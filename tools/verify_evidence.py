#!/usr/bin/env python3
"""
tools/verify_evidence.py

Automated pixel-level checks on evidence screenshots.
Complements the /verify-evidence skill (which uses Claude vision for
semantic checks) with fast, deterministic pixel analysis.

Usage:
    python3 tools/verify_evidence.py test/evidence/<slug>/
    python3 tools/verify_evidence.py test/evidence/<slug>/shot.png

Exit code: 0 if all checks pass, 1 if any fail or are unclear.
"""

import sys
import json
import pathlib
import argparse

try:
    from PIL import Image
    import numpy as np
except ImportError:
    print("ERROR: Pillow and numpy required — run: pip3 install Pillow numpy", file=sys.stderr)
    sys.exit(2)

REPO_ROOT = pathlib.Path(__file__).parent.parent

# GBA screen is always 240×160.
GBA_W, GBA_H = 240, 160

# Dialogue box: solid white band at y≥116, text pixels at y≥120.
# Calibrated from FRLG screenshots — text uses dark gray (R,G,B < 120),
# box background is near-white (all channels > 240).
TEXTBOX_ROW_START = 116
TEXTBOX_ROW_END   = 159   # inclusive of last text row at y≈157
TEXTBOX_WHITE_THRESHOLD = 100  # min pure-white pixels per row to count


def has_textbox(arr: np.ndarray) -> bool:
    """Return True if a GBA-style dialogue box is open in the image."""
    h = arr.shape[0]
    y_start = max(0, TEXTBOX_ROW_START)
    y_end   = min(h, TEXTBOX_ROW_END)
    qualifying_rows = 0
    for y in range(y_start, y_end):
        row = arr[y, 8:232]
        whites = int(np.sum((row[:, 0] > 240) & (row[:, 1] > 240) & (row[:, 2] > 240)))
        if whites >= TEXTBOX_WHITE_THRESHOLD:
            qualifying_rows += 1
    return qualifying_rows >= 5


def has_text_content(arr: np.ndarray) -> bool:
    """Return True if dark text pixels exist inside the dialogue box region.

    FRLG text uses dark gray (channels < 120), not necessarily pure black.
    """
    h = arr.shape[0]
    y_start = max(0, TEXTBOX_ROW_START + 4)
    y_end   = min(h, TEXTBOX_ROW_END)
    region = arr[y_start:y_end, 8:232]
    darks = int(np.sum((region[:, :, 0] < 120) & (region[:, :, 1] < 120) & (region[:, :, 2] < 120)))
    return darks >= 10


def diff_percent(arr_a: np.ndarray, arr_b: np.ndarray) -> float:
    """Return the percentage of pixels that differ between two screenshots."""
    if arr_a.shape != arr_b.shape:
        return 100.0
    diff = np.abs(arr_a.astype(int) - arr_b.astype(int))
    changed = np.any(diff > 8, axis=2)
    return float(changed.mean() * 100)


def is_gba_size(img: Image.Image) -> bool:
    return img.size == (GBA_W, GBA_H)


# ─── Per-screenshot check ────────────────────────────────────────────────────

def check_screenshot(png_path: pathlib.Path) -> dict:
    """Run all automated checks on a single screenshot. Returns a results dict."""
    result = {"path": str(png_path), "checks": {}, "warnings": [], "failures": []}

    try:
        img = Image.open(png_path).convert("RGB")
    except Exception as e:
        result["error"] = str(e)
        return result

    arr = np.array(img)

    if not is_gba_size(img):
        result["warnings"].append(f"Unexpected size {img.size} (expected {GBA_W}×{GBA_H})")

    box_detected = has_textbox(arr)
    result["checks"]["has_textbox"]      = box_detected
    result["checks"]["has_text_content"] = has_text_content(arr) if box_detected else None

    # Load sidecar if present
    sidecar_path = png_path.with_suffix(".json")
    if sidecar_path.exists():
        try:
            sidecar = json.loads(sidecar_path.read_text())
            result["sidecar"] = sidecar
            # Cross-validate sidecar against image — only flag genuine mismatches
            sidecar_says_open = sidecar.get("box_open", False)
            if sidecar_says_open and not box_detected:
                result["failures"].append("Sidecar says box_open=True but no text box detected in image")
            elif not sidecar_says_open and box_detected:
                result["warnings"].append("Image shows a text box but sidecar says box_open=False")
        except Exception as e:
            result["warnings"].append(f"Could not parse sidecar: {e}")
    else:
        result["warnings"].append("No JSON sidecar — capture with save_path+claim for richer evidence")

    return result


# ─── Directory-level report ──────────────────────────────────────────────────

def check_directory(evidence_dir: pathlib.Path) -> list[dict]:
    pngs = sorted(evidence_dir.glob("*.png"))
    if not pngs:
        print(f"No PNG files found in {evidence_dir}", file=sys.stderr)
        return []

    notes_path = evidence_dir / "notes.txt"
    if not notes_path.exists():
        print(f"WARNING: no notes.txt in {evidence_dir}", file=sys.stderr)

    results = []
    for png in pngs:
        results.append(check_screenshot(png))

    # Diff adjacent pairs (before/after naming convention)
    befores = [r for r in results if "before" in r["path"]]
    afters  = [r for r in results if "after"  in r["path"]]
    if befores and afters:
        # Pair by stem prefix
        for b in befores:
            stem = pathlib.Path(b["path"]).stem.replace("before", "after")
            for a in afters:
                if pathlib.Path(a["path"]).stem == stem:
                    ba = np.array(Image.open(b["path"]).convert("RGB"))
                    aa = np.array(Image.open(a["path"]).convert("RGB"))
                    pct = diff_percent(ba, aa)
                    a["checks"]["diff_from_before_pct"] = round(pct, 1)
                    if pct < 1.0:
                        a.setdefault("warnings", [])
                        a["warnings"].append(f"Only {pct:.1f}% pixels differ from before — screenshots may be identical")

    return results


# ─── Pretty-print report ─────────────────────────────────────────────────────

def print_report(results: list[dict]) -> int:
    any_fail = False
    for r in results:
        name = pathlib.Path(r["path"]).name
        print(f"\n  {name}")
        if "error" in r:
            print(f"    ERROR: {r['error']}")
            any_fail = True
            continue

        sidecar = r.get("sidecar", {})
        if sidecar.get("claim"):
            print(f"    Claim  : {sidecar['claim']}")
        if sidecar.get("text"):
            preview = sidecar["text"].replace("\n", " / ")[:80]
            print(f"    Text   : \"{preview}\"")
        if sidecar.get("box_open") is not None:
            print(f"    Sidecar: box_open={sidecar['box_open']}")

        checks = r.get("checks", {})
        for k, v in checks.items():
            if v is None:
                print(f"    — {k}: n/a")
            else:
                icon = "✓" if v else "✗"
                if isinstance(v, float):
                    print(f"    {icon} {k}: {v}")
                else:
                    print(f"    {icon} {k}: {v}")

        for f in r.get("failures", []):
            print(f"    FAIL: {f}")
            any_fail = True

        for w in r.get("warnings", []):
            print(f"    ⚠  {w}")
            if "Only" in w and "pixels differ" in w:
                any_fail = True

    return 1 if any_fail else 0


# ─── Entry point ─────────────────────────────────────────────────────────────

def main() -> None:
    ap = argparse.ArgumentParser(description="Verify evidence screenshots")
    ap.add_argument("target", help="Evidence directory or single PNG path")
    ap.add_argument("--json", action="store_true", help="Output JSON instead of human-readable text")
    args = ap.parse_args()

    target = pathlib.Path(args.target)
    if not target.is_absolute():
        target = REPO_ROOT / target

    if target.is_dir():
        results = check_directory(target)
        print(f"Evidence: {target}")
    elif target.suffix == ".png":
        results = [check_screenshot(target)]
        print(f"Evidence: {target}")
    else:
        print(f"ERROR: {target} is not a directory or .png file", file=sys.stderr)
        sys.exit(2)

    if not results:
        sys.exit(2)

    if args.json:
        print(json.dumps(results, indent=2))
        sys.exit(0)

    exit_code = print_report(results)
    total = len(results)
    fails = sum(1 for r in results if r.get("failures") or "error" in r)
    print(f"\nOverall: {'PASS' if exit_code == 0 else 'FAIL'} ({total - fails}/{total} passed)")
    sys.exit(exit_code)


if __name__ == "__main__":
    main()
