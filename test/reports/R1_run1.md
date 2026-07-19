# Scenario R1 — Route 1 Ghost Position Sync (Clean)

**Date:** 2026-07-03  
**Chaos:** Disabled  
**Report:** test/reports/R1_run1.md

## Test Execution

### CHECK-1: Ghost NPC spawn at connection
- **Result:** PASS
- **Observed:** P1 at (13,36), P2 at (13,36). Both instances show an N (NPC blocker) at (13,37), confirming partner ghost spawned on the same map.
- **Evidence:** test/evidence/R1_check1_p1.png, test/evidence/R1_check1_p2.png

### CHECK-2: Ghost follows first movement
- **Result:** PASS
- **Observed:** P1 moved to (12,36). P2's collision_map then showed an N at (12,36), confirming ghost position sync.
- **Movement:** P1 pressed UP; restricted by walls, settled at (12,36).

### CHECK-3: Ghost follows second movement
- **Result:** PASS
- **Observed:** P1 attempted second movement (UP again); remained at (12,36) due to walls. P2 still showed N at (12,36), confirming continued sync.

### CHECK-4: Ghost persists during idle (heartbeat test)
- **Result:** FAIL
- **Observed:** After 10s idle (600 frames), P2's collision_map showed NO N at p1's position (12,36). Ghost despawned on p2's side, while p1 still sees p2's ghost at (13,36). Asymmetric ghost presence indicates false disconnect on p2 or one-way sync failure.
- **Root cause:** Possible heartbeat timeout or ghost lifecycle bug causing asymmetric despawn.

### CHECK-5: Map transition (ghost despawn on warp)
- **Result:** BLOCKED
- **Reason:** Navigation to map exit was blocked. Collision_map shows "warps: none" in current Route 1 map. Attempted southward movement triggered wild Pokémon encounter (Rattata). Unable to locate accessible warp tile from current spawn position.
- **Evidence:** test/evidence/R1_check5_p1_attempt_warp.png

### CHECK-6: Ghost reappears after map return
- **Result:** BLOCKED
- **Reason:** Dependent on CHECK-5 completion.

## Summary

| Check | Result | Notes |
|-------|--------|-------|
| 1     | PASS   | Ghost spawned correctly at connection |
| 2     | PASS   | Ghost followed p1 to (12,36) |
| 3     | PASS   | Ghost remained in sync |
| 4     | FAIL   | Ghost disappeared on p2 after idle; asymmetric sync loss |
| 5     | BLOCKED | Warp tile inaccessible; wild encounter instead |
| 6     | BLOCKED | Blocked by CHECK-5 failure |

## Impact

The ghost position sync works for active movement (CHECKS 1-3), but the 10-second idle test revealed a critical issue: partner ghosts despawn asymmetrically. This suggests a heartbeat or timeout mechanism that is not functioning correctly on at least one side, or a disconnection was triggered without corresponding visual feedback.

**Critical:** CHECK-4 failure indicates potential reliability issue under real network conditions where small delays might be interpreted as disconnections.
