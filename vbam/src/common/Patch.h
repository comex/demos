#ifndef PATCH_H
#define PATCH_H

#if !JS

#include "Types.h"

bool applyPatch(const char *patchname, u8 **rom, int *size);

#endif

#endif // PATCH_H
