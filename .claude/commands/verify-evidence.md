# /verify-evidence

Verify that evidence in `test/evidence/<slug>/` actually proves what it claims.

**Usage:** `/verify-evidence <slug>`

---

## Process

### Step 1 — Run the automated pixel checker

```bash
python3 tools/verify_evidence.py test/evidence/<slug>/
```

This catches mechanical issues: missing sidecars, mismatched `box_open` state, near-identical before/after pairs. Fix any failures before proceeding.

### Step 2 — Spawn a vision verification agent

Launch a sub-agent with the full evidence directory as context. The agent sees each screenshot visually and checks it against the stated claim:

```
Agent({
  description: "Evidence quality check for <slug>",
  subagent_type: "claude",
  prompt: """
You are a QA reviewer for a Pokémon FireRed co-op multiplayer mod.
Your job is to verify that each screenshot in a test evidence directory proves its stated claim.

Evidence directory: test/evidence/<slug>/

For each .png file:
1. Read the matching .json sidecar (same name, .json extension) for the stated claim and text-box state.
2. View the .png image.
3. Assess: does the screenshot clearly prove the claim to an untrained reviewer?

Use this quality bar:
- Visibility claims: are the stated sprites clearly visible at the stated positions?
- Script/dialogue claims: is a text box open with the stated text visible on screen?
- Two-player claims: are BOTH player sprites (local + ghost) visible on screen?
- "No freeze" claims: is box_open=True in the sidecar AND dialogue text present in the image?

Return a structured report:
  <filename>.png
    Claim: <from sidecar>
    Verdict: PASS / FAIL / UNCLEAR
    Reasoning: <one or two sentences>

  Overall: PASS (N/N) or FAIL (N/N failed)

Also note any screenshots that are missing sidecars or have claims that can't be verified visually.
"""
})
```

### Step 3 — Address any failures

- **FAIL**: Re-run the scenario and re-capture the screenshot. Update the claim if the scenario changed.
- **UNCLEAR**: Add a second screenshot from a different angle or game state that makes the claim unambiguous. Update `notes.txt` to explain what to look at.
- **Missing sidecar**: Re-capture using `screenshot(save_path=..., claim=...)` so the sidecar is written automatically.

### Step 4 — Final commit

Once the automated checker and vision agent both pass:

```bash
git add test/evidence/<slug>/
git commit -m "test: add evidence for <slug>"
```

---

## What counts as PASS

| Claim type | Visual requirement | Sidecar requirement |
|------------|-------------------|---------------------|
| Script ran (no freeze) | Dialogue box visible in image | `box_open=True`, `text` matches |
| Sprite visible | Named sprite clearly identifiable on screen | — |
| Two-player both visible | Two distinct player sprites at different positions | — |
| Before/after regression | Before and after screenshots clearly differ | diff > 1% |
| Fix was applied | The broken behavior is absent in the after screenshot | — |
