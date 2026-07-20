# RB1 run 1 — co-op rival double battle (Oak's lab)

- Date: 2026-07-20
- Chaos: off
- States: p1_rivals_lab.ss1 / p2_rivals_lab.ss1
- ROM: pokefirered.gba built 2026-07-03 01:31 (includes party stash/restore fix, commit 8f6ac50520)

## Checks

| Check | Result | Observed |
|---|---|---|
| CHECK-1 battle start sync | PASS | `check_battle_sync` PASS at intro: flags 0x4040004D both, 4 battlers, identical controller funcs |
| CHECK-2 coop double battle, no crash | PASS | MULTI=Y DOUBLE=Y LINK=N both sides; both screens show local + partner trainer; ally HUD mirrored correctly (p1: Bulbasaur+Charmander, p2: Charmander+Bulbasaur) |
| CHECK-3 turn resolves in lockstep | PASS | `check_battle_sync` PASS after turn 1; PP and rival Squirtle HP identical on both screens each turn; battle won on both sides simultaneously, both starters grew to Lv 6 with identical stat screens |
| Party staging (side buffer → gPlayerParty[3] at setup) | PASS | mid-battle: p1 gPlayerParty[3] personality = 0xE6731666 (p2's Charmander), p2 gPlayerParty[3] = 0x72B31CAA (p1's Bulbasaur); both slots were 0x0 pre-battle |
| **Post-battle party restore** | **FAIL** | after both instances returned to the overworld: gPlayerParty[3] still holds the partner mon on BOTH sides; `coopPartyStashed` (gMultiplayerState+80 = 0x030015CC) still 0x01 and `coopSelectedCount` 0x01 on both — the restore code never executed |

## FAIL diagnosis (verified in code, not inferred)

`Multiplayer_OnBattleEnd()` is called only from `SetBattleEndCallbacks`
(src/battle_controller_player.c:1270). That controller function is installed
only by `PlayerHandleEndLinkBattle` — the handler for
`CONTROLLER_ENDLINKBATTLE`, which the battle engine emits **only for
`BATTLE_TYPE_LINK` / recorded-link battles** (the vanilla comment above the
function says "this is only ever used by link battles").

Coop battles are non-link by design (CLAUDE.md amendment 2026-07-03), and
regular field battles are non-link too. All of them end through
`HandleEndTurn_FinishBattle` → `FreeResetData_ReturnToOvOrDoEvolutions` →
`ReturnFromBattleToOverworld` (src/battle_main.c:5771), which never touches
the controller end-callback. So the hook is dead for every battle type it
was written for, and with it:

1. the coop party restore (this run's FAIL — partner mon stays in
   gPlayerParty[3]; with a >3-mon party the pre-battle leads would stay
   permanently overwritten),
2. the field-trainer lock release moved here by 1a1416e9a9 (partner would
   stay "Buzz off!"-locked forever after any field-trainer battle — the
   send-side beacon keeps re-carrying busy=1 because `sentBusyTrainer` is
   never cleared),
3. the post-battle auto-checkpoint save + event log entry.

Note: the local mon's exp/level looked right after the battle (Lv 5→6
observed in gPlayerParty[0] on both sides) — but that is the battle engine
writing results into gPlayerParty directly, not the restore working. The
save-block stash still holds the Lv-5 pre-battle mon until a restore runs.

Fix direction: move the `Multiplayer_OnBattleEnd()` call to
`ReturnFromBattleToOverworld` immediately after `gMain.inBattle = FALSE` —
the single point every non-link battle passes through, after evolutions and
after all battle-result writes to gPlayerParty.

## Evidence

- test/evidence/RB1_postbattle_p2_overworld.png — p2 back on overworld,
  battle completed cleanly (the sync/battle checks themselves all passed).
