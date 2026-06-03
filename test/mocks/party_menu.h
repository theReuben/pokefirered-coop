#ifndef GUARD_PARTY_MENU_H
#define GUARD_PARTY_MENU_H

// Minimal mock for native unit tests.

typedef void (*MainCallback)(void);
void SetMainCallback2(MainCallback callback);

extern u8 gSelectedOrderFromParty[MAX_FRONTIER_PARTY_SIZE];
void InitChooseHalfPartyForBattle(u8 unused);

#endif // GUARD_PARTY_MENU_H
