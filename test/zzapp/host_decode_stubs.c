/*
 * Host doubles for the decode heap and the decode owner of an app, for the
 * suites that link zzapp_api.c without sdk_compression.c. The host has no card
 * pool behind the decode heap: plain heap blocks. Weak, so a suite that
 * counts calls or fails allocations defines its own.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "sdk_compression.h"

__attribute__((weak)) uint32_t sdk_compression_app_decode_owner(uint32_t pid)
{
    return sdk_compression_app_owner_bits(pid + 1U);
}

__attribute__((weak)) void *sdk_decode_heap_alloc_owned(size_t size, uint32_t owner)
{
    (void)owner;
    return malloc(size);
}

__attribute__((weak)) void sdk_decode_heap_free(void *ptr)
{
    free(ptr);
}
