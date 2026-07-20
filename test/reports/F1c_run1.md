# F1c run 1 — field-trainer lock under chaos, new OnBattleEnd hook site

- Date: 2026-07-20
- Chaos: ON for the whole run — set_link_chaos(drop=0.3, seed=1); relay
  stats at end: dropped=297107, passed=692367 (~30% as configured)
- States: p1_forest_trainer.ss1 / p2_forest_trainer.ss1 (rebuilt for this ROM)
- ROM: with Multiplayer_OnBattleEnd hooked in ReturnFromBattleToOverworld
  (95a1f63fa8)
- Both directions run (role-swapped): pass A = p1 battles / p2 roams;
  pass B = fresh state reload, p2 battles / p1 roams

## Checks (pass A, p1 = BATTLER)

| Check | Result | Observed |
|---|---|---|
| CHECK-1 battler in battle, roamer not | PASS | p1 battle_diag: trainer battle flags 0x0C, 2 battlers; p2 on overworld |
| lock engaged on roamer under chaos | PASS | p2 `partnerHasBusyTrainer` (0x030015B2) = 0x01 during p1's battle |
| CHECK-2 approach mirrored | PASS | p2's collision_map shows p1's ghost at (6,22) adjacent to Sammy |
| CHECK-3 cone suppressed for roamer | PASS | p2 drove past the trainer to (5,20): no "!", no dialogue, no battle (evidence: F1c_check3_p2_cone_suppressed.png) |
| CHECK-4 lock released after battle | PASS | p1 `sentBusyTrainer` (0x030015B6) = 0x00 after battle (cleared by the new hook); p2 `partnerHasBusyTrainer` = 0x00 (release propagated through 30% loss — FREE packet or beacon repair) |
| CHECK-5 ghost snap | PASS | p2's map shows p1's ghost exactly at p1's real tile (6,22), no drift |

## Checks (pass B, p2 = BATTLER, fresh states)

| Check | Result | Observed |
|---|---|---|
| lock engaged on roamer | PASS | p1 `partnerHasBusyTrainer` = 0x01 during p2's battle |
| battler in battle | PASS | p2 battle_diag: trainer battle, 2 battlers |
| lock released after battle | PASS | p2 `sentBusyTrainer` = 0x00, p1 `partnerHasBusyTrainer` = 0x00 |

## Notes

- The playbook's behavioral CHECK-4 form ("Sammy spots the roamer after the
  battle") cannot fire in this fixture: the trainer-defeated flag is in the
  synced range, so once the battler wins, Sammy is defeated on BOTH
  instances and spots nobody. The memory-level reads (sentBusyTrainer /
  partnerHasBusyTrainer) are the definitive check and are what this report
  uses. Playbook should be amended to say so.
- This closes the in-emulator verification gap left by 1a1416e9a9 (lock
  release moved to OnBattleEnd) and by 95a1f63fa8 (hook site fix): the
  release now provably fires for regular field battles, in both role
  directions, under 30% packet loss.
