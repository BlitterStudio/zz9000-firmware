/*
 * Mock AmigaOS exec/io.h for the guest-client host harness.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef EXEC_IO_H
#define EXEC_IO_H

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/ports.h>

#define IOF_QUICK (1UL << 0)

struct IORequest {
    struct Message io_Message;
    struct Device *io_Device;
    struct Unit   *io_Unit;
    UWORD          io_Command;
    UBYTE          io_Flags;
    BYTE           io_Error;
};

#endif /* EXEC_IO_H */
