#ifndef GUARD_LOAD_SAVE_H
#define GUARD_LOAD_SAVE_H

// Minimal mock for native unit tests.
// Replaces include/load_save.h to avoid pulling in the SaveBlock ASLR structs
// and pokemon_storage_system.h.  Stub implementations live in stubs.c and
// mirror the real semantics: a PARTY_SIZE stash separate from gPlayerParty.

void SavePlayerParty(void);
void LoadPlayerParty(void);

#endif // GUARD_LOAD_SAVE_H
