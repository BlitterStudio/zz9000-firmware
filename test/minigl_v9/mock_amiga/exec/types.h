/*
 * Mock AmigaOS exec/types.h for ZZ9000 host transport E2E integration test
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef EXEC_TYPES_H
#define EXEC_TYPES_H

#include <stdint.h>
#include <stddef.h>

typedef uint32_t ULONG;
typedef int32_t  LONG;
typedef uint16_t UWORD;
typedef int16_t  WORD;
typedef uint8_t  UBYTE;
typedef int8_t   BYTE;
typedef void*    APTR;
typedef char*    STRPTR;

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
