# Co-op Test Playbook — scripted MCP scenarios

Step-by-step scripts for the standard two-instance co-op test scenarios,
written so that ANY model (including Haiku) can execute them with the
`gamestate` MCP tools and produce a comparable report. Follow the steps
literally; where a step says "adapt", use screenshot + get_text_state to
navigate.

## Ground rules (apply to every scenario)

- **Boot recipe:** `start_emulator("p1", savestate=<p1 state>)`, then
  `start_emulator("p2", savestate=<p2 state>)`. After each load, `wait(300)`
  on that instance before sending any input (input cooldown).
  Both fixture states have `connState=0`; the relay injects
  `PARTNER_CONNECTED` automatically within a few seconds — `wait(300)` on
  both covers it.
- **On this Windows box, `move_steps` and `advance_text` TIME OUT.** Never
  call them. Use `path_to`/`drive_to_tile` for movement and a manual loop of
  `get_text_state` + `press_button("A", hold_frames=12, release_frames=120)`
  for dialogue.
- **Transient empty reads happen.** If `read_memory`/`get_text_state`
  returns empty or nonsense once, retry before recording a FAIL.
- **Chaos variant:** where a scenario says "chaos on", call
  `set_link_chaos(drop=0.3, seed=1)` AFTER both instances are booted, and
  `set_link_chaos()` (defaults) at scenario end to disable.
- **Evidence:** save screenshots with
  `screenshot(instance_id, save_path="test/evidence/<scenario>_<step>_<p1|p2>.png", claim="<one sentence>")`.
  Take them only at the checkpoints the scenario lists — not every step.
- **Report:** write `test/reports/<scenario>_<runlabel>.md` containing: the
  scenario id, chaos settings, one line per numbered check with
  PASS/FAIL/BLOCKED and the observed value, and the verbatim tool output for
  any FAIL (paste `collision_map`/`battle_diag`/`check_battle_sync` output).
  If you get stuck, write the report with what you have and BLOCKED for the
  rest — a partial honest report beats a guessed PASS.
- **Teardown:** `stop_emulator("p1")` and `stop_emulator("p2")` at the end,
  always, even on failure.
- Do NOT modify any source files. You are generating test data only.

## Scenario R1 — Route 1 ghost position sync (+ map transition)

States: p1 = `test/lua/states/p1_route1.ss1`, p2 = `test/lua/states/p2_route1.ss1`.

1. Boot both. `collision_map("p1", radius=6)` and `collision_map("p2", radius=6)`.
   Record both players' P tiles. CHECK-1: each instance shows an `N` at (or
   within 1 tile of) the OTHER instance's P tile — the partner ghost spawned.
2. Move p1 ~5 tiles in any walkable direction (`drive_to_tile` toward a `.`
   area from the map). `wait(120)` on p2, then `collision_map` both at
   radius=6. CHECK-2: p2 shows an N exactly at p1's new P tile.
3. Repeat step 2 once more with a different direction. CHECK-3: same
   criterion. If the N lags, note by how many tiles.
4. Idle both instances for 10 s of INTERLEAVED waits: repeat 10× {
   `wait(60, "p1")`, `wait(60, "p2")` }. NEVER a single long wait on one
   instance — the other instance is frozen during it, sends no heartbeats,
   and the relay legitimately declares it disconnected (ghost despawns by
   design; this is a harness artifact, not a bug — bit us 2026-07-03).
   CHECK-4: after the interleaved idle, ghost still present on both sides.
5. Map transition: Route 1 ↔ Pallet Town is an EDGE CONNECTION (no `D`
   warp tile) — walk p1 south across the map boundary (`drive_to_tile`
   toward the bottom edge; avoid tall grass tiles where possible; if a wild
   battle starts, flee: select RUN (bottom-right of the 4-option battle
   menu: press DOWN, RIGHT, then A) and continue). `wait(240)` on p2.
   CHECK-5: p2's collision_map no longer shows p1's ghost N (despawned on
   map change).
6. Walk p1 back north across the boundary into Route 1. `wait(240)`.
   CHECK-6: p2 shows the N again at p1's current tile.
7. Screenshots at checks 1, 5, 6 (both instances).

Chaos variant R1c: same steps with chaos on. Expected: same checks pass;
CHECK-2/3 may converge within ~2 s (beacon repair) — record convergence
behaviour, FAIL only if a mismatch persists > 5 s while both stand still.

## Scenario F1 — Field-trainer lock (Viridian Forest / bug catcher Sammy)

States: p1 = `test/lua/states/p1_forest_trainer.ss1`,
p2 = `test/lua/states/p2_forest_trainer.ss1`. Both spawn at (6,23), one
tile south of Sammy's sight line.

Parameter BATTLER ∈ {p1, p2}; ROAMER = the other. Run BOTH directions
(rule: role-swapped testing).

1. Boot both.
2. `press_button("UP", instance_id=BATTLER, hold_frames=16)` once — BATTLER
   steps into Sammy's cone; the `!` intro and battle start automatically.
   Advance BATTLER's pre-battle dialogue: loop `get_text_state(BATTLER)`,
   pressing A while box_open, until `battle_diag(BATTLER)` shows an active
   battle (retry up to ~20 presses).
3. CHECK-1: `battle_diag(BATTLER)` shows a trainer battle in progress;
   `battle_diag(ROAMER)` shows NOT in battle.
4. On ROAMER: `collision_map(radius=8)`. CHECK-2: ROAMER's map shows the
   BATTLER ghost adjacent to Sammy (the approach was mirrored — ghost stands
   where the battler stands, facing the trainer).
5. Lock test: drive ROAMER across Sammy's vision cone (walk to the tile the
   BATTLER stepped on, or past the trainer). CHECK-3: ROAMER is NOT stopped
   by an `!` / does not enter a battle or dialogue (`get_text_state` stays
   closed, battle_diag stays overworld) — the busy-trainer lock suppresses
   the cone.
6. Finish the battle on BATTLER: mash A through turns (loop: if
   get_text_state box open → A; else press A; screenshot every ~30 presses
   to monitor; a lv-3/4 bug catcher falls to repeated move-1 use). It is OK
   to take up to ~80 A presses. When `battle_diag(BATTLER)` reports
   overworld again, the battle is over. If the local mon faints instead,
   record it — whiteout also ends the battle and still exercises the
   release.
7. `wait(240)` both. CHECK-4 (lock release): read memory on both sides —
   BATTLER's `sentBusyTrainer` (gMultiplayerState+58 = 0x030015B6) must be
   0x00 and ROAMER's `partnerHasBusyTrainer` (gMultiplayerState+54 =
   0x030015B2) must be 0x00 within ~2 s of the battle ending. Do NOT use
   the old behavioral form ("Sammy spots the roamer") — the
   trainer-defeated flag is in the synced range, so after the BATTLER wins,
   Sammy is defeated on BOTH instances and spots nobody (amended
   2026-07-20, F1c run 1). For the engagement direction, the same
   addresses read 0x01 during the battle.
8. CHECK-5 (ghost snap): after BATTLER's battle ended, ROAMER's map shows
   BATTLER's ghost back at BATTLER's real tile (no multi-second slide).
9. Screenshots at checks 2, 3, 4.

Chaos variant F1c: chaos on for the whole run; same checks. The lock/free
state rides the beacon, so convergence within ~2 s is a PASS.

## Scenario RB1 — Co-op rival double battle entry (Oak's lab)

States: p1 = `test/lua/states/p1_rivals_lab.ss1`,
p2 = `test/lua/states/p2_rivals_lab.ss1`. VAR_LAB=3; the rival battle
coord triggers are live on row y=8, columns x=5,6,7.

1. Boot both. (Chaos variant RB1c: enable chaos now.)
2. `path_to(6, 8, instance_id="p1")` — p1 steps on the mid trigger. The
   rival walks up; dialogue opens. Advance p1's dialogue (get_text_state +
   A loop) until the "waiting for partner" message shows or the party menu
   appears.
3. `path_to(5, 8, instance_id="p2")` — p2 steps on the left trigger.
   Advance p2's dialogue the same way. (If path_to is blocked by the ghost,
   route to x=7 instead.)
4. Both instances now open the co-op PARTY SELECTION menu. On each: the
   party has exactly one Pokémon. Adapt via screenshots: press A on the
   first mon, choose the first action ("ENTER"), then navigate to the
   CONFIRM button (usually DOWN then A) until the menu closes. Do p1 first,
   then p2.
5. Both ROMs wait for party sync, then the double battle starts. As soon as
   EITHER instance shows the battle intro (battle_diag), run
   `check_battle_sync()`. CHECK-1: it returns PASS. **If FAIL: STOP the
   scenario immediately** — take `battle_diag` of both + screenshots, write
   the report, teardown. Do not continue.
6. CHECK-2: `battle_diag` on both shows `gBattlersCount=4` and a coop/link
   double battle; neither instance crashed (no assert text on screen —
   screenshot both).
7. Play one full turn on each side: on each instance, when the action menu
   is up, choose FIGHT → move 1 → target (press A through). The turn should
   resolve on BOTH screens. CHECK-3: after the turn, `check_battle_sync()`
   still PASS and both instances show the same battle events (compare
   screenshots).
8. CHECK-4 (chaos runs only): no `INVALID ITEM` / assert crash at any point
   (the recv-ring overflow regression test).
9. Screenshots at battle start (both), after turn 1 (both).
10. Report + teardown. Do not attempt to finish the whole battle unless
    everything above passed and you have budget left; if you do finish it,
    read 8 bytes at `gPlayerParty` slots 3-5 on both sides post-battle and
    include them (species u16 at offsets +0x20 within each 100-byte mon is
    NOT readable without decryption — instead just screenshot each party
    menu post-battle showing party contents).

## Report index

Keep one line per run in `test/reports/INDEX.md`:
`| date | scenario | chaos | result | report file |`
