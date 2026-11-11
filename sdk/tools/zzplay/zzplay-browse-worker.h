/* Asynchronous worker process for service browsing and network transfers.
 *
 * Runs provider calls in a child process so GUI and audio presentation
 * stay responsive. Network library bases (bsdsocket, AmiSSL) are opened
 * per process.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_BROWSE_WORKER_H
#define ZZPLAY_BROWSE_WORKER_H

#include <stdint.h>

#include "zzplay-browse-model.h"
#include "zzplay-provider.h"

/* Start the worker process on first use. Returns 1 on success, 0 on failure. */
int zzplay_browse_worker_start(void);

/* True while a request is running in the worker process. */
int zzplay_browse_worker_active(void);

/* Signal mask to Wait() on for the worker reply port. */
uint32_t zzplay_browse_worker_signal_mask(void);

/* Send one request to the worker. Returns 1 if sent, 0 on failure.
 * Clears the cancel flag. */
int zzplay_browse_worker_send(const ZZPlayBrowseRequest *req,
                              const ZZPlayProvider *provider);

/* Cancel the active request. Sets the keep_running cancel flag so in-flight
 * network operations abort quickly. */
void zzplay_browse_worker_cancel(void);

/* Check if a completed reply is available. If so, copies the reply header to
 * *reply and returns 1. The caller owns reply->items and must release them
 * with zzplay_browse_worker_release_reply(). Returns 0 if none pending. */
int zzplay_browse_worker_take_reply(ZZPlayBrowseReply *reply);

/* Free items allocated in reply. */
void zzplay_browse_worker_release_reply(ZZPlayBrowseReply *reply);

/* Stop the worker: cancel active request, wait for reply, signal worker
 * process to exit, and wait for process shutdown. */
void zzplay_browse_worker_stop(void);

#endif /* ZZPLAY_BROWSE_WORKER_H */
