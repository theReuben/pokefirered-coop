# Scenario: Ghost NPC Visibility

**What this proves:** Both players' ghost sprites are visible on each other's screen.

**Save states:** `test/lua/states/p1_route1.ss1` (P1), `test/lua/states/p2_route1.ss1` (P2)

---

## Steps

1. Stop any running emulators.

2. Start both instances:
   ```
   start_emulator('p1', savestate='test/lua/states/p1_route1.ss1')
   start_emulator('p2', savestate='test/lua/states/p2_route1.ss1')
   ```

3. Wait 300 frames for the bootstrap ping to establish connection:
   ```
   wait(300, instance_id='p1')
   wait(300, instance_id='p2')
   ```

4. Move P2 to create separation (do NOT move P1):
   - RIGHT for `hold_frames=112` (~7 tiles)
   - DOWN for `hold_frames=64` (~4 tiles)
   ```
   press_button('RIGHT', instance_id='p2', hold_frames=112, release_frames=16)
   press_button('DOWN',  instance_id='p2', hold_frames=64,  release_frames=16)
   ```

5. Wait 120 frames for the ghost animation to reach P2's new position:
   ```
   wait(120, instance_id='p1')
   ```

6. Screenshot P1 — P2's ghost should be visible 7+ tiles right and 4 tiles below center:
   ```
   screenshot('p1',
              save_path='test/evidence/<slug>/ghost_p1.png',
              claim="P1's screen shows P2's ghost sprite at offset position after P2 moved")
   ```

7. Screenshot P2 — P1's ghost should be visible to the upper-left:
   ```
   screenshot('p2',
              save_path='test/evidence/<slug>/ghost_p2.png',
              claim="P2's screen shows P1's ghost sprite to the upper-left after separation")
   ```

---

## Quality bar

- **P1 screenshot**: Two distinct sprites visible — P1 (center) and P2's ghost (right/below). If only one sprite is visible, the ghost is not spawning.
- **P2 screenshot**: Two distinct sprites visible — P2 (center) and P1's ghost (left/above).
- Sprites must be at clearly different positions, not overlapping.

## Common failures

| Symptom | Likely cause |
|---------|-------------|
| Only one sprite on each screen | Connection never established — check relay thread is running |
| Ghosts overlap at center | Position packets not being received — check multiplayer.c |
| P2 ghost appears but P1 ghost doesn't | One-directional packet flow issue |
| Ghost visible initially then disappears | Map transition without ghost respawn |
