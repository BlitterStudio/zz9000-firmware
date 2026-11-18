/* SPDX-License-Identifier: MIT */
#ifndef ZZ9K_APP_PERF_H
#define ZZ9K_APP_PERF_H

/* Optional DIAG_SCHED request. The empty request still returns scheduler v2.
 * All words are big-endian. QUERY and RESET require flags=0; SET requires
 * no held app claims. RESET clears measurements, not the selected flags. */
#define ZZ9K_APP_PERF_TAG             0x41415250U /* AARP */
#define ZZ9K_APP_PERF_QUERY           0U
#define ZZ9K_APP_PERF_SET             1U
#define ZZ9K_APP_PERF_RESET           2U
#define ZZ9K_APP_PERF_AUDIO_NC        1U
#define ZZ9K_APP_PERF_LOCK_TIMING     2U
#define ZZ9K_APP_PERF_FLAGS           3U
#define ZZ9K_APP_PERF_REQUEST_BYTES   12U /* tag, action, flags */
#define ZZ9K_APP_PERF_REPLY_BYTES     40U
#define ZZ9K_APP_PERF_VERSION         1U
/* Reply: tag, version, flags, core-1 lock calls, lock acquisition elapsed
 * microseconds (hi/lo), maximum acquisition us, last audio ring ARM address,
 * last audio ring allocation bytes, currently held claims. The ring fields
 * are the last successful open since RESET/SET, not a live grant. */
#define ZZ9K_APP_PERF_R_FLAGS         8U
#define ZZ9K_APP_PERF_R_LOCK_CALLS    12U
#define ZZ9K_APP_PERF_R_LOCK_US_HI    16U
#define ZZ9K_APP_PERF_R_LOCK_US_LO    20U
#define ZZ9K_APP_PERF_R_LOCK_MAX_US   24U
#define ZZ9K_APP_PERF_R_RING          28U
#define ZZ9K_APP_PERF_R_RING_BYTES    32U
#define ZZ9K_APP_PERF_R_CLAIMS        36U

#endif
