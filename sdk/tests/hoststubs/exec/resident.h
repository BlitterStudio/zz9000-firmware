/*
 * Host stand-in for exec/resident.h: the fields the romtag scanner reads,
 * in the order of the AmigaOS struct Resident.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef EXEC_RESIDENT_H
#define EXEC_RESIDENT_H

#include <exec/types.h>

#define RTC_MATCHWORD 0x4AFC
#define NT_LIBRARY 9

struct Resident {
  UWORD rt_MatchWord;
  struct Resident *rt_MatchTag;
  APTR rt_EndSkip;
  UBYTE rt_Flags;
  UBYTE rt_Version;
  UBYTE rt_Type;
  BYTE rt_Pri;
  char *rt_Name;
  char *rt_IdString;
  APTR rt_Init;
};

#endif /* EXEC_RESIDENT_H */
