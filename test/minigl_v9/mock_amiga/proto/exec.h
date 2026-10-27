/*
 * Mock AmigaOS proto/exec.h for ZZ9000 host transport E2E integration test
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PROTO_EXEC_H
#define PROTO_EXEC_H

#include <exec/types.h>
#include <exec/libraries.h>

struct Library *OpenLibrary(STRPTR name, ULONG version);
void CloseLibrary(struct Library *lib);
void CacheClearE(APTR addr, ULONG length, ULONG flags);
void CopyMem(const void *src, void *dst, ULONG size);

#endif /* PROTO_EXEC_H */
