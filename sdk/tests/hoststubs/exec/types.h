/*
 * Host stand-in for the few exec types the mpega headers use, so the mpega
 * tests can compile the library's AmigaOS-free sources on a PC.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef EXEC_TYPES_H
#define EXEC_TYPES_H

#include <stdint.h>

typedef int32_t LONG;
typedef uint32_t ULONG;
typedef int16_t WORD;
typedef uint16_t UWORD;
typedef int8_t BYTE;
typedef uint8_t UBYTE;
typedef void *APTR;
typedef char *STRPTR;

#endif /* EXEC_TYPES_H */
