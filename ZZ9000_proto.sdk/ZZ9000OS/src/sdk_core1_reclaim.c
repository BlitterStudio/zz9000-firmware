/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "sdk_core1_reclaim.h"

#include "sdk_compression.h"
#include "sdk_mailbox.h"

void sdk_core1_reclaim_after_halt(taskq_t *queue)
{
    sdk_mailbox_poison_core1_audio_streams();
    (void)sdk_compression_reclaim_core1_decode();
    if (queue)
        taskq_abort_active(queue, 0U);
}
