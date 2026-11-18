/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ZZAPP_MOCK_API_H
#define ZZAPP_MOCK_API_H

#include <stdint.h>
#include "aar/app.h"

/*
 * What an app sees of the runtime on the host, with no firmware and no queue
 * behind it: an aar_api table over a small in-memory state. The transport
 * itself is modelled by mock_client.[ch].
 */
#define ZZAPP_MOCK_LOG_LINES   16u
#define ZZAPP_MOCK_LOG_BYTES   128u
#define ZZAPP_MOCK_EVENTS      64u

typedef struct zzapp_mock_app {
    int stop;                      /* aar_should_stop answers non-zero */
    char log[ZZAPP_MOCK_LOG_LINES][ZZAPP_MOCK_LOG_BYTES];
    uint32_t log_head, log_tail;   /* free-running */
    uint32_t log_dropped;
    aar_event events[ZZAPP_MOCK_EVENTS];
    uint32_t ev_head, ev_tail;
    uint32_t events_dropped;
    uint32_t width, height, pitch, format, display_flags;
    uint32_t flips;
} zzapp_mock_app;

/* Initializes a table against `app`. Caller may replace any slot with real code. */
void zzapp_mock_api_init(aar_api *api, zzapp_mock_app *app);
void zzapp_mock_time_set(uint64_t usec);
void zzapp_mock_time_advance(uint64_t usec);

/* The app's log lines, oldest first: the length copied, 0 when empty, -1 when
 * `cap` is too small. */
int zzapp_mock_app_log_read(zzapp_mock_app *app, char *buf, uint32_t cap);
/* Events are lossy: 0 queued, -1 dropped (counted). */
int zzapp_mock_app_event_push(zzapp_mock_app *app, const aar_event *ev);

#endif
