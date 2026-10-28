/*
 * Mock AmigaOS dos/dosextens.h for the guest-client host harness.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DOS_DOSEXTENS_H
#define DOS_DOSEXTENS_H

#include <dos/dos.h>
#include <exec/libraries.h>

struct Process {
    struct Node pr_Task;
    APTR        pr_WindowPtr;
};

struct DosLibrary {
    struct Library dl_LibNode;
};

struct Task *FindTask(STRPTR name);

#endif /* DOS_DOSEXTENS_H */
