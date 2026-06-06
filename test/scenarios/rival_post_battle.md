# Scenario: Rival Post-Battle (EndRivalBattle Script)

**What this proves:** After the co-op rival battle in Oak's lab, `EventScript_EndRivalBattle` runs — the rival exit dialogue appears and the player is not frozen.

**Save state:** `test/lua/states/oaks_lab_picked.ss1`  
(VAR_MAP_SCENE=3; player has picked Bulbasaur, rival took Squirtle)

**Rebuild state if stale:**
```bash
/tmp/mgba-build/mgba-headless --script tools/pick_starter.lua pokefirered.gba
```

---

## Steps

### Setup

1. Stop any running emulators.

2. Verify `oaks_lab_picked.ss1` is recent and has VAR_MAP_SCENE=3. If it's missing or stale, rebuild it first (see above).

3. Start BOTH instances from the same state:
   ```
   start_emulator('p1', savestate='test/lua/states/oaks_lab_picked.ss1')
   start_emulator('p2', savestate='test/lua/states/oaks_lab_picked.ss1')
   ```

4. Wait 300 frames for connection:
   ```
   wait(300, instance_id='p1')
   wait(300, instance_id='p2')
   ```

### Navigate to rival trigger

The rival battle triggers at tiles (5,8), (6,8), (7,8) — y=8 is 3 tiles below the player's starting position after the starter pick (approximately x=8, y=5).

5. Move both P1 and P2 to trigger tile (5,8): LEFT 3 tiles, then DOWN 3 tiles.
   ```
   # P1
   press_button('LEFT', instance_id='p1', hold_frames=16, release_frames=8)
   press_button('LEFT', instance_id='p1', hold_frames=16, release_frames=8)
   press_button('LEFT', instance_id='p1', hold_frames=16, release_frames=8)
   press_button('DOWN', instance_id='p1', hold_frames=16, release_frames=8)
   press_button('DOWN', instance_id='p1', hold_frames=16, release_frames=8)
   press_button('DOWN', instance_id='p1', hold_frames=16, release_frames=8)
   # P2 — same sequence
   ```

   The trigger fires when BOTH players are on y=8, x=5-7 with VAR_MAP_SCENE=3.

### Boss ready + party selection

6. After the trigger fires, both instances show rival dialogue. Advance past it on both:
   ```
   # Use get_text_state loop: press A while box_open=True
   # Do this for both p1 and p2
   ```

7. `waitbossstart` fires: both players must confirm ready. Press A on both when prompted.

8. `waitcoopparty` opens the party menu. Select Bulbasaur and confirm on both instances:
   - A → select Bulbasaur (slot 1)
   - A → ENTER (confirm into party)
   - B → close the slot submenu
   - DOWN → navigate to CONFIRM
   - A → confirm selection
   
   Wait for `waitpartysync` to complete (both parties exchanged). Advance any remaining dialogue with A.

### Battle

9. The battle starts. P1 controls Bulbasaur (Battler 0); P2 drives the partner (Battler 1).

   Each turn, both instances need a move selection:
   - P1: DOWN to select Vine Whip → A → A (confirm target)
   - P2: DOWN to select Vine Whip → A → A (confirm target)
   
   Repeat for each turn until the rival's Charmander faints.

10. After the battle, advance through all post-battle messages on both instances using `get_text_state` loops. **IMPORTANT:** wait for P2 to fully exit the battle and return to the overworld before screenshotting P1 — P2's ghost won't spawn on P1's map until P2 sends a position packet.

### Screenshot

11. Once P2's battle screen has cleared (P2 is back on the overworld map), wait 60 more frames, then screenshot P1:
    ```
    wait(60, instance_id='p1')
    screenshot('p1',
               save_path='test/evidence/<slug>/rival_post_battle.png',
               claim="EventScript_EndRivalBattle ran: rival exit dialogue visible, player not frozen, P2 ghost visible")
    ```

---

## Quality bar

The screenshot must show **three sprites**: player (Red), P2's ghost, and rival (Aaaaaaa). A dialogue box must be open with the rival's exit line ("Okay! I'll make my POKÉMON battle to toughen it up!" or similar). The sidecar must have `box_open=True`.

If only two sprites are visible (player + rival, no ghost): P2 has not yet returned to the overworld. Wait longer before screenshotting.

## Common failures

| Symptom | Likely cause |
|---------|-------------|
| Game freezes after party selection | `sTrainerBattleEndScript` not set — check `docooptrainerbattle` uses `goto EventScript_EndRivalBattle` |
| P2 stuck on battle move-select after P1 wins | Turn sync desync — P1 advanced past battle end before P2 resolved; advance both more aggressively |
| Only 2 sprites in screenshot (no ghost) | P2 still in battle when screenshot was taken — wait for P2 to return to overworld first |
| Rival trigger never fires | VAR_MAP_SCENE ≠ 3, or wrong tile coordinates — verify state was built correctly |
| `waitbossstart` never resolves | One instance didn't reach the trigger tile — both must be on y=8, x=5-7 |
