/*
 * Mock AmigaOS proto/graphics.h for ZZ9000 host transport E2E integration test
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PROTO_GRAPHICS_H
#define PROTO_GRAPHICS_H

#include <exec/types.h>
#include <exec/libraries.h>

struct GfxBase {
    struct Library LibNode;
};

void WaitTOF(void);

#endif /* PROTO_GRAPHICS_H */
