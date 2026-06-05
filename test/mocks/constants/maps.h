#ifndef GUARD_CONSTANTS_MAPS_H
#define GUARD_CONSTANTS_MAPS_H

// Stub for native unit tests — shadows the real constants/maps.h to avoid
// pulling in the generated map_groups.h which doesn't exist in CI.

#define MAP_GROUP(map)  ((map) >> 8)
#define MAP_NUM(map)    ((map) & 0xFF)

// Only the map constants referenced by multiplayer.c.
#define MAP_PALLET_TOWN_PROFESSOR_OAKS_LAB  (3 | (38 << 8))

#endif // GUARD_CONSTANTS_MAPS_H
