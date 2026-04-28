#!/usr/bin/env python3
"""
Generate placeholder icon files for the Pokémon FireRed Co-Op Tauri app.

Writes solid-color PNG placeholders to tauri-app/src-tauri/icons/.
Replace with a proper icon before distributing.

Run from the repo root:
    python3 tools/gen-icons.py

No external dependencies — uses only the Python standard library.
"""

import struct
import zlib
from pathlib import Path

ICONS_DIR = Path("tauri-app/src-tauri/icons")

# Pokéball red — visually distinct, easy to spot in dock/taskbar
FILL_R, FILL_G, FILL_B = 0xCC, 0x00, 0x00


def make_png(width: int, height: int, r: int, g: int, b: int) -> bytes:
    """Build a minimal valid RGBA PNG of a solid color."""
    def chunk(name: bytes, data: bytes) -> bytes:
        c = name + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    sig = b"\x89PNG\r\n\x1a\n"

    # IHDR
    ihdr_data = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    ihdr = chunk(b"IHDR", ihdr_data)

    # IDAT: each row is a filter byte (0 = None) followed by RGB triples
    raw_rows = b""
    row = b"\x00" + bytes([r, g, b]) * width
    raw_rows = row * height
    idat = chunk(b"IDAT", zlib.compress(raw_rows))

    iend = chunk(b"IEND", b"")

    return sig + ihdr + idat + iend


def make_ico(sizes: list[int]) -> bytes:
    """Build a minimal Windows ICO containing solid-color BMP bitmaps."""
    count = len(sizes)
    # ICO header: 6 bytes
    header = struct.pack("<HHH", 0, 1, count)

    images = []
    for sz in sizes:
        # 24-bit BMP DIB (no file header)
        # BITMAPINFOHEADER (40 bytes) + pixel data
        # BMP is bottom-up; rows padded to 4-byte boundaries
        row_bytes = sz * 3
        pad = (4 - row_bytes % 4) % 4
        padded_row = row_bytes + pad
        pixel_data_size = padded_row * sz

        bmpinfo = struct.pack(
            "<IIIHHIIIIII",
            40,           # biSize
            sz,           # biWidth
            sz * 2,       # biHeight (×2 for AND mask convention in ICO)
            1,            # biPlanes
            24,           # biBitCount
            0,            # biCompression
            pixel_data_size,
            0, 0, 0, 0,  # resolution, colors
        )

        # Pixels bottom-up, BGR order
        row = bytes([FILL_B, FILL_G, FILL_R]) * sz + b"\x00" * pad
        pixel_data = row * sz

        # AND mask (1 bit per pixel, all 0 = opaque), rows padded to 4 bytes
        mask_row_bytes = (sz + 31) // 32 * 4
        and_mask = b"\x00" * (mask_row_bytes * sz)

        images.append(bmpinfo + pixel_data + and_mask)

    # Directory entries: 16 bytes each
    offset = 6 + count * 16
    directory = b""
    for i, sz in enumerate(sizes):
        img_size = len(images[i])
        w = sz if sz < 256 else 0
        h = sz if sz < 256 else 0
        directory += struct.pack("<BBBBHHII", w, h, 0, 0, 1, 24, img_size, offset)
        offset += img_size

    return header + directory + b"".join(images)


def make_icns(sizes_and_types: list[tuple[int, bytes]]) -> bytes:
    """Build a minimal macOS ICNS file."""
    parts = b""
    for size, osltype in sizes_and_types:
        png_data = make_png(size, size, FILL_R, FILL_G, FILL_B)
        # ICNS chunk: 4-byte type, 4-byte total-size (including header)
        chunk_size = 8 + len(png_data)
        parts += osltype + struct.pack(">I", chunk_size) + png_data
    header = b"icns" + struct.pack(">I", 8 + len(parts))
    return header + parts


def main():
    ICONS_DIR.mkdir(parents=True, exist_ok=True)

    # PNG icons
    for name, w, h in [
        ("32x32.png", 32, 32),
        ("128x128.png", 128, 128),
        ("128x128@2x.png", 256, 256),
    ]:
        path = ICONS_DIR / name
        path.write_bytes(make_png(w, h, FILL_R, FILL_G, FILL_B))
        print(f"  {path}")

    # Windows ICO
    ico_path = ICONS_DIR / "icon.ico"
    ico_path.write_bytes(make_ico([16, 32, 48, 64, 128, 256]))
    print(f"  {ico_path}")

    # macOS ICNS (PNG payloads inside ICNS container)
    icns_path = ICONS_DIR / "icon.icns"
    icns_path.write_bytes(make_icns([
        (16,  b"icp4"),   # 16×16
        (32,  b"icp5"),   # 32×32
        (64,  b"icp6"),   # 64×64
        (128, b"ic07"),   # 128×128
        (256, b"ic08"),   # 256×256
        (512, b"ic09"),   # 512×512
    ]))
    print(f"  {icns_path}")

    print(f"\nPlaceholder icons written to {ICONS_DIR}/")
    print("Replace with real icons before distributing.")
    print("Run: cd tauri-app && npx tauri icon /path/to/source-1024x1024.png")


if __name__ == "__main__":
    main()
