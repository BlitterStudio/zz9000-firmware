/*
 * Host-test stand-in for the libvpx decoder API: only the declarations
 * sdk_video_webm.c uses. webm_backend_test.c implements them.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef STUB_VPX_DECODER_H
#define STUB_VPX_DECODER_H

#include <stdint.h>

typedef enum {
	VPX_CODEC_OK = 0,
	VPX_CODEC_ERROR = 1
} vpx_codec_err_t;

typedef enum {
	VPX_IMG_FMT_I420 = 0x102
} vpx_img_fmt_t;

typedef struct vpx_codec_iface vpx_codec_iface_t;
typedef const void *vpx_codec_iter_t;

typedef struct {
	unsigned int threads;
	unsigned int w;
	unsigned int h;
} vpx_codec_dec_cfg_t;

typedef struct {
	vpx_img_fmt_t fmt;
	unsigned int d_w;
	unsigned int d_h;
	unsigned char *planes[4];
	int stride[4];
} vpx_image_t;

typedef struct {
	vpx_codec_iface_t *iface;
	unsigned int w;
	unsigned int h;
	int pending;
	vpx_image_t img;
} vpx_codec_ctx_t;

vpx_codec_err_t vpx_codec_dec_init(vpx_codec_ctx_t *ctx,
				   vpx_codec_iface_t *iface,
				   const vpx_codec_dec_cfg_t *cfg, long flags);
vpx_codec_err_t vpx_codec_decode(vpx_codec_ctx_t *ctx, const uint8_t *data,
				 unsigned int size, void *user_priv,
				 long deadline);
vpx_image_t *vpx_codec_get_frame(vpx_codec_ctx_t *ctx, vpx_codec_iter_t *iter);
vpx_codec_err_t vpx_codec_destroy(vpx_codec_ctx_t *ctx);

#endif
