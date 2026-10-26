/* Host regression tests for the firmware module runtime allocation bridge. */
/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <errno.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zzm_runtime.h"

#define MOCK_POOL_BYTES (128U * 1024U)

extern void *memalign(size_t alignment, size_t bytes);
extern int posix_memalign(void **out, size_t alignment, size_t bytes);
extern void *aligned_alloc(size_t alignment, size_t bytes);
extern void *_malloc_r(void *reent, size_t bytes);
extern void _free_r(void *reent, void *pointer);
extern void *_calloc_r(void *reent, size_t count, size_t bytes);
extern void *_realloc_r(void *reent, void *pointer, size_t bytes);
extern void *_memalign_r(void *reent, size_t alignment, size_t bytes);
extern void abort(void);
extern void exit(int status);
extern void __assert_func(const char *file, int line, const char *func,
                          const char *expr);
extern void *_Znwj(size_t bytes);
extern void *_Znaj(size_t bytes);
extern void _ZdlPv(void *pointer);
extern void _ZdaPv(void *pointer);
extern void _ZdlPvj(void *pointer, size_t bytes);
extern void _ZdaPvj(void *pointer, size_t bytes);
extern void *_ZnwjSt11align_val_t(size_t bytes, size_t alignment);
extern void *_ZnajSt11align_val_t(size_t bytes, size_t alignment);
extern void _ZdlPvSt11align_val_t(void *pointer, size_t alignment);
extern void _ZdaPvSt11align_val_t(void *pointer, size_t alignment);
extern int __cxa_guard_acquire(uint64_t *guard);
extern void __cxa_guard_release(uint64_t *guard);
extern void __cxa_guard_abort(uint64_t *guard);

static uint8_t pool[MOCK_POOL_BYTES] __attribute__((aligned(4096)));
static uint32_t pool_used;
static unsigned alloc_calls;
static unsigned free_calls;
static unsigned log_calls;
static unsigned fault_calls;
static uint32_t alloc_owner;
static uint32_t alloc_class;
static uint32_t alloc_quota;
static uint32_t alloc_bytes;
static uint32_t alloc_alignment;
static uint32_t free_owner;
static uint32_t fault_owner;
static const char *last_log;
static const char *last_fault;
static jmp_buf fault_jump;
static int fault_jump_enabled;
static int checks;
static int failures;

#define CHECK(expr) do { \
    checks++; \
    if (!(expr)) { \
        failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    } \
} while (0)

static uint32_t mock_alloc(uint32_t owner, uint32_t memory_class, uint32_t quota,
                           uint32_t bytes, uint32_t alignment)
{
    uintptr_t address;
    uintptr_t aligned;

    alloc_calls++;
    alloc_owner = owner;
    alloc_class = memory_class;
    alloc_quota = quota;
    alloc_bytes = bytes;
    alloc_alignment = alignment;
    address = (uintptr_t)pool + pool_used;
    aligned = (address + alignment - 1U) & ~(uintptr_t)(alignment - 1U);
    if (aligned < (uintptr_t)pool || aligned + bytes > (uintptr_t)pool + sizeof(pool))
        return 0U;
    pool_used = (uint32_t)(aligned + bytes - (uintptr_t)pool);
    CHECK(aligned <= UINT32_MAX);
    return (uint32_t)aligned;
}

static int mock_free(uint32_t address, uint32_t owner)
{
    (void)address;
    free_calls++;
    free_owner = owner;
    return 1;
}

static uint32_t mock_realloc(uint32_t address, uint32_t owner, uint32_t bytes,
                             uint32_t alignment)
{
    (void)address;
    (void)owner;
    (void)bytes;
    (void)alignment;
    return 0U;
}

static void mock_log(const char *message)
{
    log_calls++;
    last_log = message;
}

static void mock_fault(const char *reason)
{
    fault_calls++;
    last_fault = reason;
    if (fault_jump_enabled)
        longjmp(fault_jump, 1);
}

static void mock_module_fault(uint32_t owner, const char *reason)
{
    fault_owner = owner;
    mock_fault(reason);
}

static uint64_t mock_time_us(void)
{
    return 0U;
}

static const zzm_core_service_t core_service = {
    ZZM_SERVICE_CORE_V1, sizeof(core_service), mock_log, mock_fault, mock_time_us,
    mock_module_fault
};
static const zzm_memory_service_t memory_service = {
    ZZM_SERVICE_MEMORY_V1, sizeof(memory_service), mock_alloc, mock_free, mock_realloc
};
static const zzm_cache_service_t cache_service = {
    ZZM_SERVICE_CACHE_V1, sizeof(cache_service), 0, 0
};

static const void *mock_get_service(uint32_t id, uint32_t min_version)
{
    if (min_version != 1U)
        return 0;
    switch (id) {
    case ZZM_SERVICE_CORE: return &core_service;
    case ZZM_SERVICE_MEMORY: return &memory_service;
    case ZZM_SERVICE_CACHE: return &cache_service;
    default: return 0;
    }
}

static const zzm_services_t services = {
    ZZM_MODULE_ABI_VERSION, sizeof(services), mock_get_service, 0, 0
};

/* This models a newlib helper: the allocation must use the reentrant bridge. */
static char *newlib_style_strdup(const char *source)
{
    size_t bytes = strlen(source) + 1U;
    char *copy = _malloc_r(0, bytes);

    if (copy)
        memcpy(copy, source, bytes);
    return copy;
}

static void reset_runtime(uint32_t quota)
{
    pool_used = 0U;
    alloc_calls = 0U;
    free_calls = 0U;
    log_calls = 0U;
    fault_calls = 0U;
    fault_owner = 0U;
    last_log = 0;
    last_fault = 0;
    CHECK(zzm_rt_bind(&services, 0x55U, ZZM_MEMORY_CLASS_MEDIA, quota) == 1);
}

static void test_allocator_entry_points(void)
{
    void *pointer;
    char *copy;

    reset_runtime(0U);
    pointer = malloc(13U);
    CHECK(pointer != 0);
    CHECK(alloc_owner == ZZM_MODULE_OWNER(0x55U));
    CHECK(alloc_class == ZZM_MEMORY_CLASS_MEDIA);
    CHECK(alloc_quota == 0U);
    CHECK(alloc_bytes > 13U);
    free(pointer);
    CHECK(free_calls == 1U);
    CHECK(free_owner == ZZM_MODULE_OWNER(0x55U));

    pointer = _malloc_r(0, 7U);
    CHECK(pointer != 0);
    _free_r(0, pointer);
    pointer = _calloc_r(0, 4U, 3U);
    CHECK(pointer != 0);
    CHECK(((uint8_t *)pointer)[0] == 0U && ((uint8_t *)pointer)[11] == 0U);
    pointer = _realloc_r(0, pointer, 24U);
    CHECK(pointer != 0);
    _free_r(0, pointer);
    pointer = _memalign_r(0, 64U, 5U);
    CHECK(pointer != 0 && ((uintptr_t)pointer & 63U) == 0U);
    _free_r(0, pointer);

    copy = newlib_style_strdup("module runtime");
    CHECK(copy != 0 && strcmp(copy, "module runtime") == 0);
    _free_r(0, copy);
}

static void test_calloc_and_realloc_failures(void)
{
    volatile size_t overflowing_count = SIZE_MAX;
    uint8_t *pointer;
    uint8_t *replacement;
    unsigned index;

    reset_runtime(32U);
    CHECK(calloc(overflowing_count, 2U) == 0);
    pointer = malloc(16U);
    CHECK(pointer != 0);
    for (index = 0U; index < 16U; ++index)
        pointer[index] = (uint8_t)(index + 1U);
    replacement = realloc(pointer, 24U);
    CHECK(replacement == 0);
    for (index = 0U; index < 16U; ++index)
        CHECK(pointer[index] == (uint8_t)(index + 1U));
    free(pointer);
}

static void test_alignment_and_quota(void)
{
    void *pointer;

    reset_runtime(0U);
    pointer = memalign(16U, 1U);
    CHECK(pointer != 0 && ((uintptr_t)pointer & 15U) == 0U);
    free(pointer);
    pointer = memalign(64U, 1U);
    CHECK(pointer != 0 && ((uintptr_t)pointer & 63U) == 0U);
    free(pointer);
    pointer = memalign(4096U, 1U);
    CHECK(pointer != 0 && ((uintptr_t)pointer & 4095U) == 0U);
    free(pointer);
    CHECK(memalign(24U, 1U) == 0);
    CHECK(posix_memalign(&pointer, 24U, 1U) != 0);
    CHECK(aligned_alloc(64U, 63U) == 0);

    reset_runtime(4U);
    CHECK(malloc(5U) == 0);
    CHECK(malloc(5U) == 0);
    CHECK(log_calls == 1U);
    CHECK(last_log != 0 && strcmp(last_log, "module allocation quota exceeded") == 0);
}

static void test_per_owner_quota_accounting(void)
{
    void *first;
    void *second;

    reset_runtime(4U);
    zzm_rt_set_context(0x56U, ZZM_MEMORY_CLASS_MEDIA, 4U);
    first = malloc(4U);
    CHECK(first != 0);
    zzm_rt_set_context(0x57U, ZZM_MEMORY_CLASS_MEDIA, 4U);
    second = malloc(4U);
    CHECK(second != 0);
    free(first);
    zzm_rt_set_context(0x56U, ZZM_MEMORY_CLASS_MEDIA, 4U);
    CHECK(malloc(4U) != 0);
    free(second);
}

static void expect_fault(void (*call)(void), const char *expected)
{
    fault_jump_enabled = 1;
    if (setjmp(fault_jump) == 0) {
        call();
        CHECK(0);
    }
    fault_jump_enabled = 0;
    CHECK(fault_calls == 1U);
    CHECK(fault_owner == 0x55U);
    CHECK(last_fault != 0 && strcmp(last_fault, expected) == 0);
}

static void call_abort(void) { abort(); }
static void call_exit(void) { exit(3); }
static void call_assert(void) { __assert_func("test", 1, "call_assert", "false"); }

static void test_faults_cpp_and_guards(void)
{
    uint64_t guard = 0U;
    void *pointer;

    reset_runtime(0U);
    expect_fault(call_abort, "module abort");
    fault_calls = 0U;
    expect_fault(call_exit, "module exit");
    fault_calls = 0U;
    expect_fault(call_assert, "module abort");

    pointer = _Znwj(8U);
    CHECK(pointer != 0);
    _ZdlPv(pointer);
    pointer = _Znaj(8U);
    CHECK(pointer != 0);
    _ZdaPv(pointer);
    pointer = _ZnwjSt11align_val_t(8U, 64U);
    CHECK(pointer != 0 && ((uintptr_t)pointer & 63U) == 0U);
    _ZdlPvSt11align_val_t(pointer, 64U);
    pointer = _ZnajSt11align_val_t(8U, 64U);
    CHECK(pointer != 0 && ((uintptr_t)pointer & 63U) == 0U);
    _ZdaPvSt11align_val_t(pointer, 64U);
    pointer = _Znwj(8U);
    CHECK(pointer != 0);
    _ZdlPvj(pointer, 8U);
    pointer = _Znaj(8U);
    CHECK(pointer != 0);
    _ZdaPvj(pointer, 8U);

    CHECK(__cxa_guard_acquire(&guard) == 1);
    __cxa_guard_abort(&guard);
    CHECK(__cxa_guard_acquire(&guard) == 1);
    __cxa_guard_release(&guard);
    CHECK(__cxa_guard_acquire(&guard) == 0);
}

int main(void)
{
    test_allocator_entry_points();
    test_calloc_and_realloc_failures();
    test_alignment_and_quota();
    test_per_owner_quota_accounting();
    test_faults_cpp_and_guards();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
