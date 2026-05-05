# Two-instance co-op scenarios

Each subdirectory here is one scenario. The Python orchestrator at
`tools/coop_harness/coop_orchestrator.py` boots two headless mGBA
instances against the same ROM, attaches `p1.lua` to one and `p2.lua`
to the other, and bridges their ring buffers (file-IPC).

**Both `p1.lua` and `p2.lua` MUST `require("_driver")` and call
`D.run(name, function(d) ... end)`.** The driver handles the bridge
tick, init wait, frame stepping, and result reporting.

## Currently shipping

- **`handshake/`** — boots both instances, p1 writes a synthetic
  `MP_PKT_POSITION` into its sendRing, p2 asserts the bridge delivered
  it and the in-ROM dispatcher routed it into `gMultiplayerState`.
  No save state, no input — runs on a fresh ROM in <10 seconds.

## Deferred (need save-state checkpoints)

These scenarios are waiting on `tools/build_save_states.lua` to land
checkpoints under `test/lua/states/`. Once that exists, each scenario
becomes a single-evening implementation:

| Scenario | Checkpoint | What it asserts |
| --- | --- | --- |
| `ghost_visibility/`     | `oaks_lab.ss1`           | p1 walks right 3 tiles → p2's ghost slot's `currentCoords` updates and `graphicsId == OBJ_EVENT_GFX_PLAYER2`. |
| `starter_lockout/`      | `oaks_lab.ss1`           | p1 picks Bulbasaur → p2's `gMultiplayerState.partnerStarterSpecies` becomes `SPECIES_BULBASAUR`; `Multiplayer_GetStarterForBall0` returns the lockout sentinel. |
| `flag_propagation/`     | `pewter_gym.ss1`         | p1 defeats Brock (script-driven) → `FLAG_DEFEATED_BROCK` and `FLAG_BADGE01_GET` become set on p2 within 5 s. |
| `boss_readiness/`       | `pewter_gym.ss1`         | both walk up to Brock → both see `bossReadyBossId == BOSS_ID_BROCK` and `partnerBossId != 0`; `Multiplayer_ScriptCheckBossStart()` returns 1 on both. |
| `randomization_match/`  | `tall_grass_route1.ss1`  | both have the same `gCoopSettings.encounterSeed`; both step into the same patch → `gEnemyParty[0].species` matches. |

## Driver helpers (from `_driver.lua`)

- `D.step(n)` — run `n` frames, bridging each
- `D.waitFor(predicate, frames, label)` — step until predicate is true or fail
- `D.assertCond(cond, msg)` — fail immediately on false
- `D.pass(name)` / `D.fail(name, reason)` — terminate explicitly
- `H.read8/16/32`, `H.write8/16/32` — memory I/O via the harness shim
- `H.recvPush(ringAddr, byte)` — push one byte into either ring (works
  for sendRing too despite the name)

## Bridge mode

`coop_orchestrator.py --bridge=inproc` (default) does the byte copy
in Python. `--bridge=partykit` is reserved for the Layer 4 smoke test
which boots a real `npx partykit dev` instead — see
`tools/coop_harness/run_relay_smoke.py`.
