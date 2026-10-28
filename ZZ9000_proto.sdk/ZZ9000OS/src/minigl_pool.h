/*
 * MiniGL card-pool budgets.  The 384 MiB MiniGL class ceiling leaves over
 * 500 MiB of the observed 846 MiB card pool for image, media, codec, video,
 * and module services.  Texture backing grows only after uploads need it.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MINIGL_POOL_H
#define MINIGL_POOL_H

#define MINIGL_CARD_POOL_LIMIT_BYTES            (384U * 1024U * 1024U)
#define MINIGL_TEXTURE_CPU_ARENA_MAX_BYTES      (128U * 1024U * 1024U)
#define MINIGL_TEXTURE_CPU_ARENA_INITIAL_BYTES  (16U * 1024U * 1024U)
#define MINIGL_TEXTURE_ARENA_GROW_BYTES         (16U * 1024U * 1024U)
#define MINIGL_RIX_TEXTURE_POOL_MAX_BYTES       (128U * 1024U * 1024U)
#define MINIGL_RIX_TEXTURE_POOL_INITIAL_BYTES   (16U * 1024U * 1024U)

#endif /* MINIGL_POOL_H */
