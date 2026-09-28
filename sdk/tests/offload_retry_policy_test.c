/*
 * Provider offload retry policy checks.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/host.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_crypto_statuses[4];
static int g_crypto_status_count;
static int g_crypto_calls;
static uint32_t g_env_fallback;
static const char *g_env_name;
static uint32_t g_set_timeout_ms;
static int g_set_timeout_calls;
static uint32_t g_alloc_flags_or;
static uint32_t g_last_alloc_length;
static int g_alloc_calls;

static int next_crypto_status(void)
{
  int index = g_crypto_calls++;
  if (index >= g_crypto_status_count) {
    index = g_crypto_status_count - 1;
  }
  return g_crypto_statuses[index];
}

int zz9k_crypto_kx(ZZ9KContext *ctx, const ZZ9KCryptoKxDesc *desc,
                   ZZ9KCryptoResult *result)
{
  (void)ctx;
  (void)desc;
  (void)result;
  return next_crypto_status();
}

int zz9k_crypto_aead(ZZ9KContext *ctx, const ZZ9KCryptoAeadDesc *desc,
                     ZZ9KCryptoResult *result)
{
  (void)ctx;
  (void)desc;
  (void)result;
  return next_crypto_status();
}

int zz9k_crypto_verify(ZZ9KContext *ctx, const ZZ9KCryptoVerifyDesc *desc,
                       int *valid)
{
  (void)ctx;
  (void)desc;
  (void)valid;
  return next_crypto_status();
}

int zz9k_alloc_shared(ZZ9KContext *ctx, uint32_t length, uint32_t alignment,
                      uint32_t flags, ZZ9KSharedBuffer *buffer)
{
  (void)ctx;
  (void)alignment;
  g_alloc_flags_or |= flags;
  g_last_alloc_length = length;
  g_alloc_calls++;
  buffer->handle = 1U;
  buffer->length = length;
  buffer->data = malloc(length ? length : 1U);
  return buffer->data ? ZZ9K_STATUS_OK : ZZ9K_STATUS_NO_MEMORY;
}

int zz9k_free_shared(ZZ9KContext *ctx, uint32_t handle)
{
  (void)ctx;
  (void)handle;
  return ZZ9K_STATUS_OK;
}

int zz9k_open(ZZ9KContext **ctx)
{
  *ctx = (ZZ9KContext *)(uintptr_t)0x1U;
  return ZZ9K_STATUS_OK;
}

void zz9k_close(ZZ9KContext *ctx)
{
  (void)ctx;
}

int zz9k_query_service(ZZ9KContext *ctx, uint32_t service_id,
                       ZZ9KServiceInfo *service)
{
  (void)ctx;
  (void)service_id;
  memset(service, 0, sizeof(*service));
  return ZZ9K_STATUS_OK;
}

uint32_t zz9k_env_u32(const char *name, uint32_t fallback)
{
  g_env_name = name;
  g_env_fallback = fallback;
  return 1234U;
}

void zz9k_set_offload_timeout_ms(ZZ9KContext *ctx, uint32_t timeout_ms)
{
  (void)ctx;
  g_set_timeout_ms = timeout_ms;
  g_set_timeout_calls++;
}

#include "../amiga/provider/zz9k_offload.c"

static void set_crypto_statuses(int a, int b, int c)
{
  g_crypto_statuses[0] = a;
  g_crypto_statuses[1] = b;
  g_crypto_statuses[2] = c;
  g_crypto_status_count = 3;
  g_crypto_calls = 0;
}

static int test_timeout_is_not_retried(void)
{
  ZZ9KCryptoKxDesc desc;
  ZZ9KCryptoResult result;
  int status;

  memset(&desc, 0, sizeof(desc));
  memset(&result, 0, sizeof(result));
  set_crypto_statuses(ZZ9K_STATUS_TIMEOUT, ZZ9K_STATUS_TIMEOUT,
                      ZZ9K_STATUS_TIMEOUT);
  status = zz9k_offload_run_kx((ZZ9KContext *)(uintptr_t)0x1U, &desc, &result);

  if (status != ZZ9K_STATUS_TIMEOUT) return 1;
  if (g_crypto_calls != 1) return 2;
  return 0;
}

static int test_busy_is_still_retried(void)
{
  ZZ9KCryptoKxDesc desc;
  ZZ9KCryptoResult result;
  int status;

  memset(&desc, 0, sizeof(desc));
  memset(&result, 0, sizeof(result));
  set_crypto_statuses(ZZ9K_STATUS_BUSY, ZZ9K_STATUS_BUSY, ZZ9K_STATUS_OK);
  status = zz9k_offload_run_kx((ZZ9KContext *)(uintptr_t)0x1U, &desc, &result);

  if (status != ZZ9K_STATUS_OK) return 1;
  if (g_crypto_calls != 3) return 2;
  return 0;
}

static int test_open_sets_timeout_from_env_parser(void)
{
  void *ctx;

  g_env_name = 0;
  g_env_fallback = 0U;
  g_set_timeout_ms = 0U;
  g_set_timeout_calls = 0;
  g_alloc_flags_or = 0U;
  g_last_alloc_length = 0U;
  g_alloc_calls = 0;

  ctx = zz9k_offload_open(0);
  if (!ctx) return 1;
  if (!g_env_name || strcmp(g_env_name, "ZZ9K_OFFLOAD_TIMEOUT_MS") != 0) {
    zz9k_offload_close(ctx);
    return 2;
  }
  if (g_env_fallback != 250U) {
    zz9k_offload_close(ctx);
    return 3;
  }
  if (g_set_timeout_calls != 1 || g_set_timeout_ms != 1234U) {
    zz9k_offload_close(ctx);
    return 4;
  }
  if (g_alloc_calls != 1 ||
      (g_alloc_flags_or & ZZ9K_ALLOC_HOST_WINDOW) == 0U ||
      g_last_alloc_length != 32U) {
    zz9k_offload_close(ctx);
    return 5;
  }

  zz9k_offload_close(ctx);
  return 0;
}

int main(void)
{
  int result;

  result = test_timeout_is_not_retried();
  if (result) {
    printf("timeout retry policy failed: %d\n", result);
    return 10 + result;
  }

  result = test_busy_is_still_retried();
  if (result) {
    printf("busy retry policy failed: %d\n", result);
    return 20 + result;
  }

  result = test_open_sets_timeout_from_env_parser();
  if (result) {
    printf("offload timeout env setup failed: %d\n", result);
    return 30 + result;
  }

  printf("offload_retry_policy_test: passed\n");
  return 0;
}
