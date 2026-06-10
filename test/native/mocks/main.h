#ifndef GUARD_MOCKS_MAIN_H
#define GUARD_MOCKS_MAIN_H

// Re-export the real header so struct Main has the same layout in every TU.
// multiplayer.c already pulls in the real include/main.h transitively (via
// quoted includes inside include/*.h, which resolve relative to include/);
// a divergent stub here would give stubs.c a different gMain layout and
// out-of-bounds field accesses at run time.
#include "../../../include/main.h"

#endif
