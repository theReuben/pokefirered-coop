# /evidence

Capture screenshot evidence proving a fix works. Saves to `test/evidence/<slug>/`.

**Usage:** `/evidence <slug> [description]`

- `slug` — kebab-case directory name under `test/evidence/` (e.g. `fix-trainer-freeze-0116`)
- `description` — optional one-liner of what was fixed

---

## Process

### 1. Set up the directory

Create `test/evidence/<slug>/` and start a `notes.txt`:

```
Fix: <commit hash or branch>
<One-sentence description of what changed>
```

### 2. Start emulators and navigate to the scenario

Choose the appropriate save state and scenario recipe from `test/scenarios/`:

| Scenario | State | Recipe |
|----------|-------|--------|
| Ghost NPC visible | `p1_route1.ss1` + `p2_route1.ss1` | `test/scenarios/ghost_visibility.md` |
| Oak starter scene | `oaks_lab.ss1` | `test/scenarios/oak_starter_scene.md` |
| Rival post-battle | `oaks_lab_picked.ss1` | `test/scenarios/rival_post_battle.md` |
| Other | pick appropriate state | describe steps inline |

### 3. Capture each screenshot with a claim

Always provide both `save_path` and `claim` so a JSON sidecar is written automatically:

```
screenshot(instance_id='p1',
           save_path='test/evidence/<slug>/<name>.png',
           claim='<one sentence: what this proves>')
```

The sidecar (`<name>.json`) captures `box_open`, `text`, and timestamp at the moment of capture.

### 4. Advance dialogue reliably

Use `get_text_state` to drive A-presses instead of counting frames:

```
# Keep pressing A while text is printing or waiting
get_text_state(instance_id='p1')
# if box_open=True: press_button('A', hold_frames=12, release_frames=60), repeat
# if box_open=False: screenshot is ready to capture
```

For co-op scenarios: advance BOTH instances out of any dialogue/battle before the final screenshot so P2's ghost is visible on P1's screen.

### 5. Run the automated verifier

```bash
python3 tools/verify_evidence.py test/evidence/<slug>/
```

Fix any warnings before committing. The verifier checks:
- Text box detected in image matches sidecar `box_open` field
- Screenshots with a "before" counterpart differ by > 1%
- JSON sidecar present for each PNG

### 6. Update notes.txt

For each screenshot, add:
- What bug/change is evidenced
- What to look at and why it proves the fix
- Any limitations (what couldn't be captured, and why)

### 7. Commit

```bash
git add test/evidence/<slug>/
git commit -m "test: add evidence for <slug>"
```

---

## Quality bar

A screenshot is valid evidence if an untrained reviewer can verify the fix by looking at it:

- **Visibility fixes**: ≥ 2 distinct sprites clearly at different positions
- **Script/dialogue fixes**: text box open with relevant text; `box_open=True` in sidecar
- **Two-player fixes**: BOTH instances should appear (screenshot P1 after P2 returns to overworld)
- **Regression fixes**: provide a before screenshot (broken state) alongside the after

If a screenshot fails this bar, adjust the scenario and re-capture before committing.

---

## Available scenario recipes

See `test/scenarios/` for step-by-step instructions on specific scenarios.
Each recipe documents exact navigation, timing, and the quality bar for that scenario.
