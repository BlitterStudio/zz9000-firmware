/*
 * Mock AmigaOS exec/execbase.h for the guest-client host harness.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef EXEC_EXECBASE_H
#define EXEC_EXECBASE_H

#include <exec/types.h>
#include <exec/libraries.h>

#define CACRF_CLEARD (1UL << 11)
#define CACRF_ClearD CACRF_CLEARD

struct Task {
    struct Node tc_Node;
};

struct ExecBase {
    struct Library LibNode;
};

#endif /* EXEC_EXECBASE_H */
