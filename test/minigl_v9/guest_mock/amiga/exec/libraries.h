/*
 * Mock AmigaOS exec/libraries.h for the guest-client host harness.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef EXEC_LIBRARIES_H
#define EXEC_LIBRARIES_H

#include <exec/types.h>

#define NT_LIBRARY 0
#define NT_DEVICE  3
#define NT_MSGPORT 4

struct Node {
    struct Node *ln_Succ;
    struct Node *ln_Pred;
    UWORD        ln_Type;
    BYTE         ln_Pri;
    char        *ln_Name;
};

struct List {
    struct Node *lh_Head;
    struct Node *lh_Tail;
    struct Node *lh_TailPred;
    UBYTE        lh_Type;
};

struct Library {
    struct Node lib_Node;
    UBYTE        lib_Flags;
    UBYTE        lib_pad;
    UWORD        lib_Version;
    UWORD        lib_Revision;
    ULONG        lib_OpenCnt;
};

#endif /* EXEC_LIBRARIES_H */
