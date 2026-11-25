/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Internal vocabulary of the app runtime that is not wire: the lifecycle
 * states of a claim as the host and the loader keep them, and the fault codes
 * the exception handlers hand to the host. The wire is zzapp_proto.h and
 * aar/transport.h; this header defines only the runtime's internal vocabulary.
 *
 * States are the AAR_STATE_* values that go into AAR_CB_STATE. Fault codes
 * are the runtime's classification (watchdog, forced stop and core restart
 * are runtime causes, not CPU faults); zzapp_fault_to_aar() turns one into
 * the AAR_FAULT_* class of the control block's fault record.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZAPP_STATE_H
#define ZZAPP_STATE_H

#include <stdint.h>

#include "aar/app.h"

/* Lifecycle states: AAR_CB_STATE values. */
#define ZZAPP_ST_IDLE              AAR_STATE_IDLE
#define ZZAPP_ST_LOADING           AAR_STATE_LOADING
#define ZZAPP_ST_STARTING          AAR_STATE_STARTING
#define ZZAPP_ST_RUNNING           AAR_STATE_RUNNING
#define ZZAPP_ST_STOPPING          AAR_STATE_STOPPING
#define ZZAPP_ST_FAULTED           AAR_STATE_FAULTED
/* A client claimed the record and has not loaded an image yet. */
#define ZZAPP_ST_CLAIMED           AAR_STATE_CLAIMED

/* Fault codes the exception handlers and the watchdogs pass to the host. */
#define ZZAPP_FAULT_NONE           0u
#define ZZAPP_FAULT_DATA           1u
#define ZZAPP_FAULT_PREFETCH       2u
#define ZZAPP_FAULT_UNDEF          3u
#define ZZAPP_FAULT_WATCHDOG       4u
#define ZZAPP_FAULT_FORCED         5u
#define ZZAPP_FAULT_WATCHDOG_RESTART 6u

/* The AAR_FAULT_* class of a fault code. `fsr` is the data fault status
 * register of a data abort: an alignment fault (status 0b00001) is reported
 * as AAR_FAULT_ALIGN. */
static inline uint32_t zzapp_fault_to_aar(uint32_t fault, uint32_t fsr)
{
	switch (fault) {
	case ZZAPP_FAULT_NONE:
		return AAR_FAULT_NONE;
	case ZZAPP_FAULT_DATA:
		return (fsr & 0x040fu) == 0x1u ? AAR_FAULT_ALIGN : AAR_FAULT_DATA;
	case ZZAPP_FAULT_PREFETCH:
		return AAR_FAULT_FETCH;
	case ZZAPP_FAULT_UNDEF:
		return AAR_FAULT_ILLEGAL;
	default:
		return AAR_FAULT_OTHER;
	}
}

/* Per-claim limits the runtime reports. */
#define ZZAPP_FD_COUNT             16

#endif /* ZZAPP_STATE_H */
