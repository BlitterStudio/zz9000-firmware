/*
 * Mock AmigaOS exec/memory.h for the guest-client host harness.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef EXEC_MEMORY_H
#define EXEC_MEMORY_H

#include <exec/types.h>

#define MEMF_CHIP     (1UL << 2)
#define MEMF_ANY      (1UL << 0)
#define MEMF_PUBLIC   (1UL << 1)
#define MEMF_CLEAR    (1UL << 16)

APTR  AllocMem(ULONG size, ULONG flags);
void  FreeMem(APTR block, ULONG size);
APTR  AllocVec(ULONG size, ULONG flags);
void  FreeVec(APTR block);

#endif /* EXEC_MEMORY_H */
