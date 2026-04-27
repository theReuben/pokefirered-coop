# Encounter System (Phase 4 Reference)

How wild encounters are generated in pokeemerald-expansion (FRLG mode) and
the strategy for seeded co-op randomization.

---

## Key Structs

```c
// include/wild_encounter.h
struct WildPokemon {
    u8  minLevel;
    u8  maxLevel;
    u16 species;
};

struct WildPokemonInfo {
    u8                        encounterRate;
    const struct WildPokemon *wildPokemon;  // points into ROM const array
};

struct WildEncounterTypes {
    const struct WildPokemonInfo *landMonsInfo;      // 12 slots
    const struct WildPokemonInfo *waterMonsInfo;     //  5 slots
    const struct WildPokemonInfo *rockSmashMonsInfo; //  5 slots
    const struct WildPokemonInfo *fishingMonsInfo;   // 10 slots
    const struct WildPokemonInfo *hiddenMonsInfo;    //  3 slots
};

struct WildPokemonHeader {
    u8 mapGroup;
    u8 mapNum;
    const struct WildEncounterTypes encounterTypes[TIMES_OF_DAY_COUNT]; // [4]
};
```

### Slot counts (include/constants/wild_encounter.h)

| Area       | Slots |
|------------|-------|
| Land       | 12    |
| Water      | 5     |
| Rock Smash | 5     |
| Fishing    | 10    |
| Hidden     | 3     |

`TIMES_OF_DAY_COUNT = 4` (morning, day, evening, night).
In FireRed, `OW_TIME_OF_DAY_ENCOUNTERS = FALSE`
(`include/config/overworld.h:95`), so `GetTimeOfDayForEncounters()` always
returns `TIME_OF_DAY_DEFAULT` (= 0 = `TIME_MORNING`).
Only `encounterTypes[0]` is ever used.

---

## Global Tables

```c
// src/data/wild_encounters.h (auto-generated — do NOT edit)
extern const struct WildPokemonHeader gWildMonHeaders[];
```

`gWildMonHeaders` is **const ROM data** — it cannot be modified at runtime.

- 389 total entries across all games (Emerald + FireRed + LeafGreen)
- **132 FIRERED entries** are compiled into the FIRERED build
- Terminated by a `MAP_UNDEFINED` sentinel entry
- FIRERED data guarded with `#ifdef FIRERED` throughout the file

---

## Encounter Generation Flow

`StandardWildEncounter()` → `TryGenerateWildMon()`:

```c
// src/wild_encounter.c:540
CreateWildMon(wildMonInfo->wildPokemon[wildMonIndex].species, level);
```

`GenerateFishingWildMon()` (separate function, same pattern):

```c
// src/wild_encounter.c:547
u16 wildMonSpecies = wildMonInfo->wildPokemon[wildMonIndex].species;
```

**Hook points:** Both calls read `wildPokemon[wildMonIndex].species` from
the const ROM table. We intercept here to substitute a randomized species.

---

## Route 1 (FireRed) Example

```c
// src/data/wild_encounters.h:8466
const struct WildPokemon sRoute1_FireRed_LandMons[] = {
    { 3, 3, SPECIES_PIDGEY },   // slot 0 — 20% chance
    { 3, 3, SPECIES_RATTATA },  // slot 1 — 20%
    { 3, 3, SPECIES_PIDGEY },   // slot 2 — 10%
    { 3, 3, SPECIES_RATTATA },  // slot 3 — 10%
    { 2, 2, SPECIES_PIDGEY },   // slot 4 — 10%
    { 2, 2, SPECIES_RATTATA },  // slot 5 — 10%
    { 3, 3, SPECIES_PIDGEY },   // slot 6 —  5%
    { 3, 3, SPECIES_RATTATA },  // slot 7 —  5%
    { 4, 4, SPECIES_PIDGEY },   // slot 8 —  4%
    { 4, 4, SPECIES_RATTATA },  // slot 9 —  4%
    { 5, 5, SPECIES_PIDGEY },   // slot 10 — 1%
    { 4, 4, SPECIES_RATTATA },  // slot 11 — 1%
};
```

Level ranges are preserved across all slots; only species are replaced.

---

## Randomizer Design (Phase 4 Implementation)

### Constraint

`gWildMonHeaders` and the `WildPokemon[]` arrays are `const` ROM data.
We cannot write to them at runtime.

### Approach: per-slot hash (no EWRAM pre-allocation)

Rather than copying the tables into EWRAM, derive each randomized species
deterministically on demand using a hash of `(seed, tablePtr, slotIndex)`:

```c
// Called from TryGenerateWildMon / GenerateFishingWildMon
// in place of wildMonInfo->wildPokemon[idx].species
u16 Multiplayer_GetRandomizedSpecies(u32 tableAddr, u8 slotIndex)
{
    // xorshift32 keyed on (seed XOR table address XOR slot)
    // Same seed + same (table, slot) always yields the same species.
    u32 state = gCoopSettings.encounterSeed ^ tableAddr ^ (u32)slotIndex;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return sValidSpecies[state % sValidSpeciesCount];
}
```

Both players' ROMs compute identical species from identical inputs because:
- `gCoopSettings.encounterSeed` is synced via `SEED_SYNC` packet
- `tableAddr` is the ROM address of the same `const WildPokemon[]` array
- `slotIndex` is the same slot index chosen by `ChooseWildMonIndex_*`

### Valid species pool

`NUM_SPECIES` = 1573 (`SPECIES_EGG`, `include/constants/species.h:1693`).
Valid species: IDs 1–1572. Filter out:
- `SPECIES_NONE` (0)
- `SPECIES_EGG`
- Mega evolutions and regional forms that never appear in the wild
  (implementation details in `src/multiplayer.c`)

For v1, use species 1–493 (Gen I–IV national dex) as the wild pool
to avoid form-species complexity; expand later.

### Hook locations

1. **`TryGenerateWildMon`** (`src/wild_encounter.c:540`):
   Wrap `wildMonInfo->wildPokemon[wildMonIndex].species`

2. **`GenerateFishingWildMon`** (`src/wild_encounter.c:547`):
   Wrap `wildMonInfo->wildPokemon[wildMonIndex].species`

No other overworld encounter paths exist in FRLG mode (no DexNav, no mass
outbreak in FireRed, no battle pyramid/pike in FRLG).

### Seed lifecycle

1. Host generates seed on session start → stores in `gCoopSettings.encounterSeed`
2. Host sends `SEED_SYNC` packet to guest on connect
3. Guest receives seed → stores in `gCoopSettings.encounterSeed`
4. Both ROMs now produce identical per-slot species for every encounter

The seed is **not** saved to flash; a new seed is generated each co-op session.
The on/off toggle IS saved (see `SAVE_SECTION_COOP_SETTINGS`).

---

## Total Encounter Slot Count (FIRERED build)

| Area       | FireRed arrays | Slots each | Max slots |
|------------|----------------|------------|-----------|
| Land       | ~90            | 12         | ~1080     |
| Water      | ~50            | 5          | ~250      |
| Fishing    | ~50            | 10         | ~500      |
| Rock Smash | ~20            | 5          | ~100      |
| Hidden     | ~5             | 3          | ~15       |
| **Total**  |                |            | **~1945** |

With hash-on-demand, no EWRAM table is needed. Both players compute the
same species from the same `(seed, tableAddr, slotIndex)` inputs.

---

## CoopSettings Struct (reference)

```c
// include/constants/multiplayer.h
struct CoopSettings {
    u8  randomizeEncounters : 1;  // 1 = on (default), 0 = off
    u8  padding : 7;
    u32 encounterSeed;            // session-only; not saved
};
extern struct CoopSettings gCoopSettings;
```
