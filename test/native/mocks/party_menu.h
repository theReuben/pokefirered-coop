#ifndef GUARD_PARTY_MENU_H
#define GUARD_PARTY_MENU_H

// Minimal mock for native unit tests.

typedef void (*MainCallback)(void);
void SetMainCallback2(MainCallback callback);

extern u8 gSelectedOrderFromParty[MAX_FRONTIER_PARTY_SIZE];
void InitChooseHalfPartyForBattle(u8 unused);

// Recorders set by the native stubs (test/native/stubs.c) so tests can observe
// the coop party-select cancel guard re-opening the menu.
extern u32 gStubReopenCount;
extern MainCallback gStubLastCallback;

#endif // GUARD_PARTY_MENU_H
