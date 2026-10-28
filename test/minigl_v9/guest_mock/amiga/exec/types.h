/*
 * Mock AmigaOS exec/types.h for the guest-client host harness.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef EXEC_TYPES_H
#define EXEC_TYPES_H

#include <stdint.h>
#include <stddef.h>
/* ULONG/LONG are pointer-sized (as on the Amiga, where ULONG == a pointer
 * == 32 bits): guest tests walk the dispatch table and tag lists as arrays
 * of ULONG, which is only well-defined when sizeof(ULONG) == sizeof(void *).
 * Fixed-width needs use UWORD/UBYTE or the stdint types. */
typedef unsigned long ULONG;
typedef long          LONG;
typedef uint16_t UWORD;
typedef int16_t  WORD;
typedef const char *CONST_STRPTR;
typedef uint8_t  UBYTE;
typedef int8_t   BYTE;
typedef void    *APTR;
typedef char    *STRPTR;
typedef int      BOOL;
typedef void     VOID;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef NULL
#define NULL ((void *)0)
#endif

#endif /* EXEC_TYPES_H */
