#ifndef GUARD_MAIN_H
#define GUARD_MAIN_H

typedef void (*MainCallback)(void);
typedef void (*IntrCallback)(void);
typedef void (*IntrFunc)(void);

// Minimal stub — only the fields used by multiplayer.c are present.
struct Main
{
    u8 inBattle : 1;
    u8 _pad     : 7;
};

extern struct Main gMain;

void SetMainCallback2(MainCallback callback);
void SetVBlankCallback(IntrCallback callback);

#endif
