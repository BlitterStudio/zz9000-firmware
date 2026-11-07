/*
 * Mock AmigaOS proto/expansion.h for ZZ9000 host transport E2E integration test
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PROTO_EXPANSION_H
#define PROTO_EXPANSION_H

#include <exec/types.h>
#include <libraries/configvars.h>

struct ExpansionBase {
    struct Library LibNode;
};

struct ConfigDev *FindConfigDev(struct ConfigDev *old, ULONG manuf, ULONG product);

#endif /* PROTO_EXPANSION_H */
