/*
 * JPEG decode bridge for the shared ZZ9000 picture viewer.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ9K_JPEG_VIEW_H
#define ZZ9K_JPEG_VIEW_H

#include "zz9k-picture-viewer.h"

int zz9k_jpeg_decode_viewer_image(ZZ9KContext *ctx,
                                  const ZZ9KSurface *framebuffer,
                                  const char *path,
                                  ZZ9KPictureViewerImage *image);

#endif /* ZZ9K_JPEG_VIEW_H */
