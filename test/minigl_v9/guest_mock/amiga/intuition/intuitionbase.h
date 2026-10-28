/*
 * Mock AmigaOS intuition/intuitionbase.h for the guest-client host harness.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef INTUITION_INTUITIONBASE_H
#define INTUITION_INTUITIONBASE_H

#include <exec/types.h>
#include <exec/libraries.h>

struct IntuitionBase {
    struct Library LibNode;
};

#endif /* INTUITION_INTUITIONBASE_H */
