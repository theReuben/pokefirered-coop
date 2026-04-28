#ifndef GUARD_STRINGS_H
#define GUARD_STRINGS_H

// Stub: shadows ../include/strings.h (GBA-specific, uses u8 type) for
// native unit tests. The system string.h pulls in <strings.h> on Linux;
// with -I mocks first this stub is found instead, preventing GBA types
// from being required before they are defined.

#endif // GUARD_STRINGS_H
