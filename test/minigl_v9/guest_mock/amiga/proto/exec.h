/*
 * Mock AmigaOS proto/exec.h for the guest-client host harness.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PROTO_EXEC_H
#define PROTO_EXEC_H

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <exec/ports.h>

struct Library *OpenLibrary(STRPTR name, ULONG version);
void CloseLibrary(struct Library *lib);
void CacheClearE(APTR addr, ULONG length, ULONG flags);
void CopyMem(const void *src, void *dst, ULONG size);
void CopyMemQuick(const void *src, void *dst, ULONG size);
void Forbid(void);
void Permit(void);
void Disable(void);
void Enable(void);
LONG AllocSignal(LONG sig);
void FreeSignal(LONG sig);
struct Message *GetMsg(struct MsgPort *port);
void WaitPort(struct MsgPort *port);
void ReplyMsg(struct Message *msg);
LONG OpenDevice(STRPTR name, ULONG unit, struct IORequest *req, ULONG flags);
void CloseDevice(struct IORequest *req);
BYTE DoIO(struct IORequest *req);

#endif /* PROTO_EXEC_H */
