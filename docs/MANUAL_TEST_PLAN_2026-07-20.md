# Manual test plan — evening of 2026-07-20

Everything below runs on the build cut from `main` today — **use `v0.1.28`**
(the earlier `v0.5.2` tag was off-scheme and has been deleted; v0.1.28
additionally contains the evening's coop AI lockstep fix, see section F).
The release workflow also redeploys the PartyKit relay, so both app and
relay are current.

## Getting the build

1. GitHub → Releases → `v0.1.28` (it lands as a **draft** — that's the
   workflow default; the assets are downloadable for you as repo owner, or
   hit "Publish" first if you prefer).
2. Install/unpack on both machines (or two copies on one machine).
3. If you'd rather run from source: the new ROM is already bundled at
   `tauri-app/src-tauri/rom/pokefirered.gba` — `npm run tauri dev` from
   `tauri-app/`.
4. **Start fresh saves** for the co-op session unless a test says otherwise
   — several fixes change save-adjacent state.

## What changed since you last played (v0.1.22-era)

All 7 bugs from your June list are code-complete, and two of them plus the
co-op battle party handling got fixed *again* today after live emulator
verification caught a dead hook (the battle-end code never ran for non-link
battles — so if you saw "fixed" things misbehave after battles before,
that's why).

## The checklist (in this order — later items build on earlier state)

### A. Session + overworld basics (5 min)
- [ ] Host / join with room code; both connect; ghosts appear and track.
- [ ] **#15**: have P2 join while P1 is mid-cutscene (e.g. the Oak escort on
      a fresh save): P2's ghost of P1 should *walk along* with the cutscene,
      not stick at the door.

### B. Starters + rival (10 min)
- [ ] Pick different starters; each sees the other's ball gray out; rival
      gets the third.
- [ ] ⚠️ KNOWN OPEN: do NOT race to pick the *same* starter simultaneously —
      the `starter_denied` message never reaches the ROM (bridge drops it),
      so the slower player silently desyncs. (Or DO try it if you want to
      see the bug live; just restart the session after.)
- [ ] **#16/#17**: win the co-op rival battle in Oak's lab. Afterward: the
      parcel → Pokédex scene should fire for BOTH players on next lab
      entry; the rival battle must NOT re-trigger; no phantom "mom heal"
      scene for the loser-side player.
- [ ] **Textbox scroll fix**: during the co-op rival double battle intro,
      the message box should slide in once and stay put — no continuous
      upward scrolling.

### C. Field trainers (10 min) — verified in-emulator today, confirm on real relay
- [ ] **#18a**: P1 walks into a route trainer's sight line and battles.
      While the battle runs, P2 walks through the same trainer's cone: no
      "!" and no second battle; talking to the trainer gives "Buzz off!".
- [ ] **#18a release**: after P1's battle ends, the lock clears (P2 can be
      spotted by OTHER trainers normally; the beaten trainer stays beaten
      for both — that part is flag sync, also worth confirming).
- [ ] **#18b**: at the moment P1 is spotted, P2 (same map) sees the
      trainer's "!" and walk-up mirrored next to P1's ghost.
- [ ] Ghost snap: when P1's battle ends, P1's ghost on P2's screen should
      snap to P1's real position, not slide across the map.

### D. Co-op gym battle — the party restore (15 min, the big one)
Today's headline fix. Best done at Brock with parties of **4+ mons each**
(catch a few on Route 2/Viridian Forest first — also exercises **#20**:
wild/trainer mons should have proper level-up movesets, no single-move
sweepers).
- [ ] Both ready up at Brock; party selection opens; each selects a SUBSET
      (e.g. 2 of 4 mons, NOT the first slots — pick slots 3-4 to stress the
      reorder).
- [ ] Double battle runs in lockstep: same damage rolls, same order, both
      screens agree every turn.
- [ ] **#19**: watch for the enemy attacking different targets on the two
      screens (left/right) — that desync is fixed; both screens must show
      the same target.
- [ ] **AFTER the battle — check your party menu carefully:**
      - all your original mons are back, in their original slots,
      - the mons that fought kept their exp/level-ups/HP/status,
      - your partner's mons are GONE from your party,
      - non-participants are untouched.
- [ ] Save, reload, and re-check the party (persistence of the restore).
- [ ] Disconnect variant if you have patience: kill one app mid-gym-battle;
      the other side's AI takes over after ~30 s; when the battle ends the
      party must STILL restore correctly.

### E. Coop battle AI targeting — the evening's desync fix (10 min)
Your report from this afternoon's session: the enemy attacked by screen
position, not mon, and the battle diverged. Root cause was deeper than the
June target fix — the AI's *deliberation itself* consumed lockstep RNG in
mirrored order, desyncing the whole stream. Fixed and MCP-verified (clean +
30% packet loss); this is the live confirmation.
- [ ] In any coop double battle (rival redo or Brock), let the enemy act
      for **at least 4-5 turns** — don't rush the kill. Every enemy move,
      target, and damage number must match on both screens (same mon hit —
      it appears on OPPOSITE sides of the two screens; that's correct).
- [ ] Especially watch enemy STATUS moves (Growl/Tail Whip/leer): those
      were the draws that desynced the stream. Stat-fall messages must name
      the same mons on both screens.
- [ ] If either screen ever shows a different move/target/HP than the
      other: note the turn number and both screens' text — that's a repro
      we can replay in the harness.
- [ ] **Known open issue (don't file as new):** if one player gets stuck on
      "Waiting for partner… Press B to cancel" at a boss trigger while the
      other is already in party selection, that's the boss-ready chaos
      window (found tonight, not yet fixed). Workaround: stuck player
      presses B, BOTH walk off the triggers, then step on again within a
      second or two of each other.

### F. Follower ghost diagnostic (2 min, when you see it)
- [ ] **#21**: if the partner has a follower Pokémon, its ghost should be a
      Pokémon sprite. The fix makes a bad sprite *despawn* rather than
      render as a tree/NPC. So: follower ghost briefly VANISHING =
      expected/fixed path; a TREE walking behind the ghost = the other
      hypothesis (tile-slot reuse) — screenshot it if you see it, that's
      the diagnostic we need.

## If something looks wrong

Note what you did, both screens' behavior, and (if reproducible) the
sequence — the MCP harness can usually replay it from a save state the
next session. Reports from today's automated runs are in `test/reports/`
(RB1 runs 1-2, F1c) if you want the memory-level details of what was
verified where.
