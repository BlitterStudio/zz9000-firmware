/*
 * Card-playback admission for codec-aware audio streams.
 *
 * The AX pump converts a source rate to 48 kHz and plays S16LE mono or
 * stereo. Ring leases are a narrower subset (see
 * zz9k_audio_ring_rate_known): a lease period must be an integer number
 * of frames that fits in one 48 kHz period. 11025 needs a 40 ms pump
 * quantum, and 88200/96000 source periods do not fit the lease scratch.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef AUDIO_STREAM_CARD_H
#define AUDIO_STREAM_CARD_H

#include <stdint.h>

#include "sdk_mailbox.h"

/* Rates the stream pump can convert to 48 kHz, including the bypass. */
static inline int audio_pump_source_rate_ok(uint32_t rate)
{
	return rate == 8000U || rate == 11025U || rate == 12000U ||
	       rate == 16000U || rate == 22050U || rate == 24000U ||
	       rate == 32000U || rate == 44100U || rate == 48000U ||
	       rate == 88200U || rate == 96000U;
}

/* BeginEx format gate. OK, UNSUPPORTED (a real format this codec cannot
 * produce), or BAD_REQUEST (not a sample format). */
static inline uint16_t audio_stream_begin_ex_format_status(uint32_t codec,
                                                           uint32_t format)
{
	if (codec == SDK_AUDIO_CODEC_FLAC) {
		if (format == SDK_AUDIO_SAMPLE_FORMAT_S16LE ||
		    format == SDK_AUDIO_SAMPLE_FORMAT_S16BE ||
		    format == SDK_AUDIO_SAMPLE_FORMAT_S32BE)
			return SDK_STATUS_OK;
		if (format == SDK_AUDIO_SAMPLE_FORMAT_S32LE)
			return SDK_STATUS_UNSUPPORTED;
		return SDK_STATUS_BAD_REQUEST;
	}
	if (codec == SDK_AUDIO_CODEC_VORBIS) {
		if (format == SDK_AUDIO_SAMPLE_FORMAT_S16LE ||
		    format == SDK_AUDIO_SAMPLE_FORMAT_S16BE)
			return SDK_STATUS_OK;
		if (format == SDK_AUDIO_SAMPLE_FORMAT_S32LE ||
		    format == SDK_AUDIO_SAMPLE_FORMAT_S32BE)
			return SDK_STATUS_UNSUPPORTED;
		return SDK_STATUS_BAD_REQUEST;
	}
	if (format == SDK_AUDIO_SAMPLE_FORMAT_S32LE ||
	    format == SDK_AUDIO_SAMPLE_FORMAT_S32BE)
		return SDK_STATUS_UNSUPPORTED;
	if (format != SDK_AUDIO_SAMPLE_FORMAT_S16LE &&
	    format != SDK_AUDIO_SAMPLE_FORMAT_S16BE)
		return SDK_STATUS_BAD_REQUEST;
	return SDK_STATUS_OK;
}

/* Play admission once a session exists. Rate 0 is the prebuffer rule.
 * FLAC and Vorbis also require mono/stereo and a pump rate; MP3 keeps
 * its historical gates (S16LE, rate known). */
static inline uint16_t audio_stream_card_play_status(uint32_t codec,
                                                     uint32_t format,
                                                     uint32_t channels,
                                                     uint32_t rate)
{
	if (codec != SDK_AUDIO_CODEC_MP3 &&
	    codec != SDK_AUDIO_CODEC_FLAC &&
	    codec != SDK_AUDIO_CODEC_VORBIS)
		return SDK_STATUS_UNSUPPORTED;
	if (rate == 0U)
		return SDK_STATUS_BAD_REQUEST;
	if (format != SDK_AUDIO_SAMPLE_FORMAT_S16LE)
		return SDK_STATUS_UNSUPPORTED;
	if (codec == SDK_AUDIO_CODEC_FLAC ||
	    codec == SDK_AUDIO_CODEC_VORBIS) {
		if (channels != 1U && channels != 2U)
			return SDK_STATUS_UNSUPPORTED;
		if (!audio_pump_source_rate_ok(rate))
			return SDK_STATUS_UNSUPPORTED;
	}
	return SDK_STATUS_OK;
}

#endif /* AUDIO_STREAM_CARD_H */
