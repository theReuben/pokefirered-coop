# Scenario: Oak Starter Scene

**What this proves:** `ChooseStarterScene` plays correctly — Oak appears near the lab entrance (y≈11), walks north, and the scene completes without freezing.

**Save state:** `test/lua/states/oaks_lab.ss1`

---

## Steps

1. Stop any running emulators.

2. Start P1 only:
   ```
   start_emulator('p1', savestate='test/lua/states/oaks_lab.ss1')
   ```

3. Wait 30 frames for the map to load:
   ```
   wait(30, instance_id='p1')
   ```

4. Screenshot — Oak should be visible near the center/lower portion of the lab, NOT hidden behind the counter at the top:
   ```
   screenshot('p1',
              save_path='test/evidence/<slug>/oak_scene_start.png',
              claim="Oak is at the entrance area (y≈11) when ChooseStarterScene starts, not behind the counter (y=3)")
   ```

5. Advance through the scene. Use `get_text_state` to drive A-presses through dialogue:
   ```
   # Check state, press A while box is open
   get_text_state('p1')
   # If box_open=True: press_button('A', hold_frames=12, release_frames=60)
   # Repeat until box_open=False
   ```
   Wait up to 300 frames total for the Oak walk + player walk animation.

6. Screenshot — player should be standing near the Poké Ball table; "Be patient, [name]!" or the starter prompt should have appeared:
   ```
   screenshot('p1',
              save_path='test/evidence/<slug>/oak_scene_complete.png',
              claim="Scene ran to completion: player walked up, starter prompt visible, no freeze")
   ```

---

## Quality bar

- **Start screenshot**: Oak sprite is in the lower half of the lab frame (center/entrance area). If Oak is at the very top behind the counter, the bug is not fixed.
- **Complete screenshot**: Player has moved north from entrance; a dialogue/prompt box is open (or scene has advanced to the Poké Ball table). The game is NOT frozen (box_open=True or field is clear).

## Notes on guest-specific regression

The root cause was a race between var sync and `OnTransition`. Full proof requires a two-instance test where P2 enters the lab with `VAR_MAP_SCENE=0` and receives the `var_set` packet to 1 mid-session. The single-player scenario here verifies the defensive fix (re-positioning Oak at scene start regardless of transition timing).

## Common failures

| Symptom | Likely cause |
|---------|-------------|
| Oak at top of screen (y≈3) at scene start | `ReadyOakForStarterScene` not called at top of `ChooseStarterScene` |
| Scene freezes partway through | Oak's walk target y is invalid (check `ChooseStarterScene` walk distance) |
| No Oak sprite visible at all | Object not added; check `addobject` call in `ReadyOakForStarterScene` |
