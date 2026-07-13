/*
 * ZZ9000AX capture-period conversion and status helpers.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ_AUDIO_CAPTURE_H
#define ZZ_AUDIO_CAPTURE_H

#include <stdint.h>

#define ZZ_AUDIO_CAPTURE_INPUT_FRAMES 960U
#define ZZ_AUDIO_CAPTURE_PERIODS      8U
#define ZZ_AUDIO_CAPTURE_RESIDENT_PERIODS (ZZ_AUDIO_CAPTURE_PERIODS - 1U)

#define ZZ_AUDIO_CONFIG_PLAY          0x0001U
#define ZZ_AUDIO_CONFIG_RECORD        0x0002U
#define ZZ_AUDIO_CONFIG_MASK          0x0003U

#define ZZ_AUDIO_RX_STATUS_CAPABLE       0x8000U
#define ZZ_AUDIO_RX_STATUS_PERIOD_SHIFT  12U
#define ZZ_AUDIO_RX_STATUS_PERIOD_MASK   0x7000U
#define ZZ_AUDIO_RX_STATUS_SEQUENCE_MASK 0x0fffU

/*
 * Convert one 48 kHz S16LE stereo DMA period in place to output_frames
 * S16BE stereo frames. Invalid frame counts fall back to the native 960.
 * Returns the number of output frames written.
 */
uint16_t zz_audio_capture_convert(uint8_t *period, uint16_t output_frames);

static inline uint16_t zz_audio_rx_status_pack(uint8_t period,
                                               uint16_t sequence)
{
	return (uint16_t)(ZZ_AUDIO_RX_STATUS_CAPABLE |
	    (((uint16_t)period & (ZZ_AUDIO_CAPTURE_PERIODS - 1U)) <<
	     ZZ_AUDIO_RX_STATUS_PERIOD_SHIFT) |
	    (sequence & ZZ_AUDIO_RX_STATUS_SEQUENCE_MASK));
}

static inline uint8_t zz_audio_rx_status_period(uint16_t status)
{
	return (uint8_t)((status & ZZ_AUDIO_RX_STATUS_PERIOD_MASK) >>
	                 ZZ_AUDIO_RX_STATUS_PERIOD_SHIFT);
}

static inline uint16_t zz_audio_rx_status_sequence(uint16_t status)
{
	return status & ZZ_AUDIO_RX_STATUS_SEQUENCE_MASK;
}

static inline uint16_t zz_audio_rx_sequence_distance(uint16_t newer,
                                                      uint16_t older)
{
	return (newer - older) & ZZ_AUDIO_RX_STATUS_SEQUENCE_MASK;
}

static inline uint8_t zz_audio_capture_completed_period(
    uint32_t transfer_count, uint32_t bytes_per_period)
{
	uint32_t writer_period =
	    (transfer_count / bytes_per_period) % ZZ_AUDIO_CAPTURE_PERIODS;

	return (uint8_t)((writer_period + ZZ_AUDIO_CAPTURE_PERIODS - 1U) %
	                 ZZ_AUDIO_CAPTURE_PERIODS);
}

static inline uint8_t zz_audio_capture_period_distance(uint8_t newer,
                                                        uint8_t older)
{
	return (uint8_t)((newer - older) &
	                 (ZZ_AUDIO_CAPTURE_PERIODS - 1U));
}

#endif /* ZZ_AUDIO_CAPTURE_H */
