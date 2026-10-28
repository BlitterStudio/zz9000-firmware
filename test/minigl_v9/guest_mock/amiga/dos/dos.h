/*
 * Mock AmigaOS dos/dos.h for the guest-client host harness.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DOS_DOS_H
#define DOS_DOS_H

#include <exec/types.h>

typedef LONG BPTR;
typedef LONG BSTR;

#define MODE_NEWFILE   1006
#define MODE_OLDFILE   1005
#define MODE_READWRITE 1004

#define OFFSET_BEGINNING (-1)
#define OFFSET_CURRENT    0
#define OFFSET_END        1

LONG DeleteFile(STRPTR name);
BPTR Open(STRPTR name, LONG accessMode);
LONG Close(BPTR file);
LONG Read(BPTR file, APTR buffer, LONG length);
LONG Write(BPTR file, APTR buffer, LONG length);
LONG Seek(BPTR file, LONG position, LONG offset);
BPTR Output(void);
BPTR Input(void);
LONG GetVar(CONST_STRPTR name, STRPTR buffer, LONG size, ULONG flags);

struct DateStamp {
    ULONG ds_Days;
    ULONG ds_Minute;
    ULONG ds_Tick;
};

void DateStamp(struct DateStamp *stamp);

#endif /* DOS_DOS_H */
