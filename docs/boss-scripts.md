# Boss Scripts — FRLG Gym Leaders, Elite Four, Champion

## Overview

This document describes the script structure for all boss encounters in FireRed/LeafGreen and documents the hook points used by the co-op boss readiness protocol (Phase 5).

---

## Script Command: `trainerbattle_single`

All 8 gym leaders use this macro (defined in `asm/macros/event.inc:763`):

```
trainerbattle_single TRAINER, intro_text, lose_text[, event_script[, NO_MUSIC]]
```

When `event_script` and `NO_MUSIC` are supplied (as all gym leaders do), this expands to:

```
trainerbattle TRAINER_BATTLE_CONTINUE_SCRIPT_NO_MUSIC, LOCALID_NONE, TRAINER,
              intro_text, lose_text, event_script, ...
```

### Execution flow

1. `ScrCmd_trainerbattle` is called — sets up `gTrainerBattleParameter` and jumps to
   `EventScript_TryDoNormalTrainerBattle` (in `src/battle_setup.c:BattleSetup_ConfigureTrainerBattle`).
2. After the battle, the defeated callback (`event_script`) runs automatically via
   `ScrCmd_gotobeatenscript` / `ScrCmd_gotopostbattlescript`.
3. The defeated script sets `FLAG_DEFEATED_X` and `FLAG_BADGENN_GET`.

### Elite Four command: `trainerbattle_no_intro`

The Elite Four use `trainerbattle_no_intro`, which maps to `TRAINER_BATTLE_SINGLE_NO_INTRO_TEXT`
and jumps directly to `EventScript_DoNoIntroTrainerBattle` (no intro text shown).

---

## All Boss Encounter Locations

| # | Leader   | Map file                              | Trainer ID                     | Defeat flag              | Badge flag           |
|---|----------|---------------------------------------|-------------------------------|--------------------------|----------------------|
| 1 | Brock    | `PewterCity_Gym_Frlg/scripts.inc`     | `TRAINER_LEADER_BROCK`         | `FLAG_DEFEATED_BROCK`    | `FLAG_BADGE01_GET`   |
| 2 | Misty    | `CeruleanCity_Gym_Frlg/scripts.inc`   | `TRAINER_LEADER_MISTY`         | `FLAG_DEFEATED_MISTY`    | `FLAG_BADGE02_GET`   |
| 3 | Lt. Surge| `VermilionCity_Gym_Frlg/scripts.inc`  | `TRAINER_LEADER_LT_SURGE`      | `FLAG_DEFEATED_LT_SURGE` | `FLAG_BADGE03_GET`   |
| 4 | Erika    | `CeladonCity_Gym_Frlg/scripts.inc`    | `TRAINER_LEADER_ERIKA`         | `FLAG_DEFEATED_ERIKA`    | `FLAG_BADGE04_GET`   |
| 5 | Koga     | `FuchsiaCity_Gym_Frlg/scripts.inc`    | `TRAINER_LEADER_KOGA`          | `FLAG_DEFEATED_KOGA`     | `FLAG_BADGE05_GET`   |
| 6 | Sabrina  | `SaffronCity_Gym_Frlg/scripts.inc`    | `TRAINER_LEADER_SABRINA`       | `FLAG_DEFEATED_SABRINA`  | `FLAG_BADGE06_GET`   |
| 7 | Blaine   | `CinnabarIsland_Gym_Frlg/scripts.inc` | `TRAINER_LEADER_BLAINE`        | `FLAG_DEFEATED_BLAINE`   | `FLAG_BADGE07_GET`   |
| 8 | Giovanni | `ViridianCity_Gym_Frlg/scripts.inc`   | `TRAINER_LEADER_GIOVANNI`      | `FLAG_DEFEATED_LEADER_GIOVANNI` | `FLAG_BADGE08_GET` |

### Elite Four and Champion

| Entity    | Map file                                         | Defeat flag                 |
|-----------|--------------------------------------------------|-----------------------------|
| Lorelei   | `PokemonLeague_LoreleisRoom_Frlg/scripts.inc`    | `FLAG_DEFEATED_LORELEI`     |
| Bruno     | `PokemonLeague_BrunosRoom_Frlg/scripts.inc`      | `FLAG_DEFEATED_BRUNO`       |
| Agatha    | `PokemonLeague_AgathasRoom_Frlg/scripts.inc`     | `FLAG_DEFEATED_AGATHA`      |
| Lance     | `PokemonLeague_LancesRoom_Frlg/scripts.inc`      | `FLAG_DEFEATED_LANCE`       |
| Champion  | `PokemonLeague_ChampionsRoom_Frlg/scripts.inc`   | (rival-dependent; no single flag) |

All defeat flags fall within `SYNC_FLAG_BOSSES_START`–`SYNC_FLAG_BOSSES_END` (0x4B0–0x4BC), so they are automatically synced to the partner on set.

---

## Boss IDs for BOSS_READY Packets

The `MP_PKT_BOSS_READY` packet carries a 1-byte boss ID. Values are ordered by game progression:

| Boss ID | Boss      |
|---------|-----------|
| 1       | Brock     |
| 2       | Misty     |
| 3       | Lt. Surge |
| 4       | Erika     |
| 5       | Koga      |
| 6       | Sabrina   |
| 7       | Blaine    |
| 8       | Giovanni  |
| 9       | Lorelei   |
| 10      | Bruno     |
| 11      | Agatha    |
| 12      | Lance     |
| 13      | Champion  |

These constants will be defined in `include/constants/multiplayer.h` when step 5.2 is implemented.

---

## Co-op Boss Readiness Protocol (Phase 5)

### V1 simplification (Phase 5 plan)

Both players fight the gym leader **independently**. Flag sync ensures that once either player sets `FLAG_DEFEATED_X`, the partner also has the flag (via FULL_SYNC or incremental FLAG_SET sync). Badge-gated map checks (e.g. unlock Route 3 after Brock) pass for both players after the flag is set by either one.

This avoids real-time battle synchronization while still preserving the co-op feel: a late-joining player inherits their partner's progress.

### Script modification approach (Step 5.3)

The modification to each gym leader script wraps the existing `trainerbattle_single` call with a co-op readiness check:

```
GymLeaderBoss_Script::
    @ If disconnected, go straight to battle (single-player fallback).
    checkvar VAR_COOP_CONNECTED
    goto_if_eq VAR_RESULT, 0, BossScript_DirectBattle

    @ Connected: send BOSS_READY and wait for partner.
    special Multiplayer_ScriptSendBossReady  @ sets VAR_BOSS_BATTLE_STATE=0, sends pkt
    msgbox BossWaitText, MSGBOX_DEFAULT       @ "Waiting for partner..."
    waitbuttonpress
    @ Poll until BOSS_START arrived (Multiplayer_Update sets VAR_BOSS_BATTLE_STATE=1).
    goto_if_ne VAR_BOSS_BATTLE_STATE, 1, BossScript_WaitLoop
    @ Fall through to battle.

BossScript_DirectBattle::
    trainerbattle_single TRAINER_LEADER_BROCK, IntroText, DefeatText, DefeatedScript, NO_MUSIC
    ...
```

This approach uses existing `special`, `msgbox`, and `goto_if_ne` script commands; no new bytecode commands are needed for v1.

For `VAR_BOSS_BATTLE_STATE` and `VAR_COOP_CONNECTED`, unused script variable slots from `include/constants/vars.h` will be repurposed.
