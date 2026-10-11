/*
 * Host-test stand-in for libvpx's decoder interface getters.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef STUB_VP8DX_H
#define STUB_VP8DX_H

#include "vpx/vpx_decoder.h"

vpx_codec_iface_t *vpx_codec_vp8_dx(void);
vpx_codec_iface_t *vpx_codec_vp9_dx(void);

#endif
