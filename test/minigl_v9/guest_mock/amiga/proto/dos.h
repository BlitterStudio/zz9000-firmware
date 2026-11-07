/*
 * Mock AmigaOS proto/dos.h for the guest-client host harness.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PROTO_DOS_H
#define PROTO_DOS_H

#include <exec/types.h>
#include <exec/libraries.h>
#include <dos/dos.h>
#include <dos/dosextens.h>

extern struct DosLibrary *DOSBase;

#endif /* PROTO_DOS_H */
