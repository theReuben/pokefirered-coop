// Shadows the repo's GBA include/malloc.h for native test builds.
// On mingw (Windows host) the system <string.h> does `#include <malloc.h>`,
// which -I resolution would otherwise satisfy with the GBA header (whose u32
// types don't exist natively).  Guard matches the repo header so any later
// direct include is a no-op.
#ifndef GUARD_ALLOC_H
#define GUARD_ALLOC_H

#include <stdlib.h>

#define Alloc       malloc
#define Free        free

#endif // GUARD_ALLOC_H
