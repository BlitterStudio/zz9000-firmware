/*
 * Sliceable wait for the runner's sleep_us. One clock read per pass and a
 * capped delay slice keep a stop request visible even when the deadline is
 * far away, and avoid the old double-read underflow where the deadline
 * passing between the check and the subtraction produced a days-long
 * SDL_Delay with no stop check inside.
 *
 * SPDX-License-Identifier: MIT
 */
#include "host_internal.h"

#include <SDL.h>

void host_sleep_until(uint64_t deadline)
{
    while (!host_stop_requested()) {
        uint64_t now = host_time_us();
        uint64_t left;

        if (now >= deadline)
            return;
        left = deadline - now;
        if (left > 10000ull)
            SDL_Delay(10); /* capped slice: stop is re-checked next pass */
        else
            host_pump();
    }
}
