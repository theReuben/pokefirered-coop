#ifndef GUARD_MAIN_H
#define GUARD_MAIN_H

typedef void (*MainCallback)(void);
typedef void (*IntrCallback)(void);
typedef void (*IntrFunc)(void);

void SetMainCallback2(MainCallback callback);
void SetVBlankCallback(IntrCallback callback);

#endif
