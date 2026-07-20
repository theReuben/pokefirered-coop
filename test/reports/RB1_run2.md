# RB1 run 2 — co-op rival double battle, after the OnBattleEnd hook fix

- Date: 2026-07-20
- Chaos: off
- States: p1_rivals_lab.ss1 / p2_rivals_lab.ss1 (rebuilt for this ROM)
- ROM: pokefirered.gba with Multiplayer_OnBattleEnd moved to
  ReturnFromBattleToOverworld (fixes the dead link-only hook found in run 1)

## Checks

| Check | Result | Observed |
|---|---|---|
| CHECK-1 battle start sync | PASS | `check_battle_sync` PASS at intro (flags 0x4040004D, 4 battlers, identical controllers) |
| CHECK-2 coop double battle, no crash | PASS | MULTI=Y DOUBLE=Y LINK=N both; ally HUD mirrored |
| CHECK-3 turn lockstep | PASS | identical PP/HP each turn; battle won simultaneously; both starters to Lv 6 |
| Party staging at setup | PASS | mid-battle gPlayerParty[3]: p1=0xE6731666 (partner Charmander), p2=0x72B31CAA (partner Bulbasaur) |
| **Post-battle party restore** | **PASS** | back on overworld: gPlayerParty[3] = 0x00000000 on BOTH (partner evicted); `coopPartyStashed`/`coopSelectedCount` (0x030015CC/CD) both 0x00 (restore ran and cleared); gPlayerParty[0] personality unchanged (0x72B31CAA / 0xE6731666) with post-battle state intact (level byte 6, maxHP 23/22 — battle results written back through the stash) |

## Notes

- Same scenario, states, and memory checks as run 1; the only change is the
  hook site (battle_main.c `ReturnFromBattleToOverworld`, right after
  `gMain.inBattle = FALSE`).
- This also revives the field-trainer lock release and the post-battle
  checkpoint save that lived in the same dead hook. Lock release
  (F1 CHECK-4) not re-verified in-emulator this run — the beacon recv-side
  repair covers the partner side regardless; a full F1 pass under chaos is
  still worth doing when convenient.

## Evidence

- test/evidence/RB1_run2_p1_postbattle_restored.png
