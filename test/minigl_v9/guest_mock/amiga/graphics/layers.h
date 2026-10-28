/*
 * Mock AmigaOS graphics/layers.h for the guest-client host harness.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef GRAPHICS_LAYERS_H
#define GRAPHICS_LAYERS_H

#include <exec/types.h>
#include <exec/libraries.h>

struct Layer_Info {
    struct Layer *top_layer;
};

extern struct Library *LayersBase;

void LockLayer(LONG dummy, struct Layer *layer);
void UnlockLayer(struct Layer *layer);

#endif /* GRAPHICS_LAYERS_H */
