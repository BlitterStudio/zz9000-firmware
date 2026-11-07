/*
 * Mock AmigaOS exec/ports.h for the guest-client host harness.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef EXEC_PORTS_H
#define EXEC_PORTS_H

#include <exec/types.h>
#include <exec/libraries.h>

#define PA_SIGNAL 0

struct MsgPort {
    struct Node  mp_Node;
    UBYTE        mp_Flags;
    UBYTE        mp_SigBit;
    void        *mp_SigTask;
    struct List  mp_MsgList;
};

struct Message {
    struct Node     mn_Node;
    struct MsgPort *mn_ReplyPort;
    UWORD           mn_Length;
};

#endif /* EXEC_PORTS_H */
