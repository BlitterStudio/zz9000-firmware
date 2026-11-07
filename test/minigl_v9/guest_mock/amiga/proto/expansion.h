/*
 * Mock AmigaOS proto/expansion.h for the guest-client host harness.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PROTO_EXPANSION_H
#define PROTO_EXPANSION_H

#include <exec/types.h>
#include <exec/libraries.h>
#include <libraries/configvars.h>

struct ExpansionBase {
    struct Library LibNode;
};

struct ConfigDev *FindConfigDev(struct ConfigDev *old, ULONG manuf, ULONG product);

#endif /* PROTO_EXPANSION_H */
