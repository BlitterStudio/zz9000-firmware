#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include "zzm_runtime.h"

void abort(void);
void exit(int status);

/* Some standalone newlib headers expose errno but not its POSIX constants. */
#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ENOSYS
#define ENOSYS 38
#endif

#define ZZM_RT_ALLOC_MAGIC 0x5a52544dU

struct zzm_rt_allocation {
    uint32_t magic;
    uint32_t address;
    uint32_t bytes;
    uint32_t owner;
    uint32_t memory_class;
    uint32_t quota;
};

static const zzm_services_t *runtime;
static const zzm_core_service_t *core;
static const zzm_memory_service_t *memory;
static const zzm_cache_service_t *cache;
static zzm_allocation_context_t context;

#define ZZM_RT_MAX_CONTEXTS 16U
typedef struct zzm_rt_usage {
    uint32_t owner;
    uint32_t memory_class;
    uint32_t quota;
    uint32_t used;
} zzm_rt_usage_t;
static zzm_rt_usage_t usages[ZZM_RT_MAX_CONTEXTS];
static uint32_t quota_reported;
static uint32_t bound_module_id;

const uint32_t zzm_rt_identity = 0x5a5a4d32U;
void *__dso_handle __attribute__((weak)) = &__dso_handle;

static int is_power_of_two(size_t value)
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

static void quota_log_once(void)
{
    if (!quota_reported && core && core->log) {
        core->log("module allocation quota exceeded");
        quota_reported = 1U;
    }
}

static zzm_rt_usage_t *usage_for(uint32_t owner, uint32_t memory_class,
                                  uint32_t quota, int create)
{
    zzm_rt_usage_t *empty = 0;
    uint32_t i;

    for (i = 0U; i < ZZM_RT_MAX_CONTEXTS; i++) {
        zzm_rt_usage_t *usage = &usages[i];

        if (usage->owner == owner && usage->memory_class == memory_class &&
            usage->quota == quota)
            return usage;
        if (!empty && (usage->owner == 0U || usage->used == 0U))
            empty = usage;
    }
    if (create && empty) {
        empty->owner = owner;
        empty->memory_class = memory_class;
        empty->quota = quota;
        empty->used = 0U;
    }
    return create ? empty : 0;
}

int zzm_rt_bind(const zzm_services_t *services, uint32_t module_id,
                uint32_t memory_class, uint32_t quota)
{
    if (!services ||
        (services->abi_version != ZZM_MODULE_ABI_VERSION &&
         services->abi_version != ZZM_V3_ABI_MAJOR) ||
        services->size < sizeof(*services) || !services->get_service)
        return 0;
    runtime = services;
    core = (const zzm_core_service_t *)services->get_service(ZZM_SERVICE_CORE,
                                                               ZZM_SERVICE_CORE_V1);
    memory = (const zzm_memory_service_t *)services->get_service(ZZM_SERVICE_MEMORY,
                                                                   ZZM_SERVICE_MEMORY_V1);
    cache = (const zzm_cache_service_t *)services->get_service(ZZM_SERVICE_CACHE,
                                                                 ZZM_SERVICE_CACHE_V1);
    if (!core || !memory || !core->log || !core->fault || !memory->alloc ||
        !memory->free || !memory->realloc)
        return 0;
    bound_module_id = module_id;
    context.owner = ZZM_MODULE_OWNER(module_id);
    context.memory_class = memory_class;
    context.quota = quota;
    memset(usages, 0, sizeof(usages));
    quota_reported = 0U;
    return 1;
}

void zzm_rt_set_context(uint32_t owner, uint32_t memory_class, uint32_t quota)
{
    context.owner = owner;
    context.memory_class = memory_class;
    context.quota = quota;
}

void zzm_rt_get_context(zzm_allocation_context_t *out)
{
    if (out)
        *out = context;
}

const void *zzm_rt_get_service(uint32_t id, uint32_t min_version)
{
    return runtime && runtime->get_service ?
        runtime->get_service(id, min_version) : 0;
}

void zzm_rt_log(const char *message)
{
    if (core && core->size >= offsetof(zzm_core_service_t, log_limited) +
                                   sizeof(core->log_limited) &&
        core->log_limited)
        core->log_limited(bound_module_id, 0U, message);
    else if (core && core->log)
        core->log(message);
}

void zzm_rt_fault(const char *reason)
{
    if (core && core->size >= offsetof(zzm_core_service_t, module_fault) +
                                   sizeof(core->module_fault) &&
        core->module_fault)
        core->module_fault(bound_module_id, reason);
    else if (core && core->fault)
        core->fault(reason);
}

void zzm_rt_dcache_clean(uint32_t address, uint32_t length)
{
    if (cache && cache->dcache_clean)
        cache->dcache_clean(address, length);
}

void zzm_rt_dcache_invalidate(uint32_t address, uint32_t length)
{
    if (cache && cache->size >=
            offsetof(zzm_cache_service_t, dcache_invalidate_owned) +
            sizeof(cache->dcache_invalidate_owned) &&
        cache->dcache_invalidate_owned)
        (void)cache->dcache_invalidate_owned(context.owner, address, length);
}
int zzm_rt_dcache_clean_owned(uint32_t owner, uint32_t address, uint32_t length)
{
    if (cache && cache->size >=
            offsetof(zzm_cache_service_t, dcache_clean_owned) +
            sizeof(cache->dcache_clean_owned) &&
        cache->dcache_clean_owned)
        return cache->dcache_clean_owned(owner, address, length);
    return 0;
}

void zzm_rt_barrier(void)
{
    const zzm_graphics_hw_service_t *graphics =
        (const zzm_graphics_hw_service_t *)zzm_rt_get_service(
            ZZM_SERVICE_GRAPHICS_HW, ZZM_SERVICE_GRAPHICS_HW_V3);

    if (graphics && graphics->size >=
            offsetof(zzm_graphics_hw_service_t, barrier) +
            sizeof(graphics->barrier) && graphics->barrier)
        graphics->barrier();
}

uint64_t zzm_rt_time_us(void)
{
    if (core && core->size >= offsetof(zzm_core_service_t, time_us) +
                               sizeof(core->time_us) && core->time_us)
        return core->time_us();
    return 0U;
}
void *memalign(size_t alignment, size_t bytes)
{
    struct zzm_rt_allocation *header;
    uintptr_t aligned;
    uint32_t address;
    size_t total;

    if (!memory || !is_power_of_two(alignment) || alignment < sizeof(void *) ||
        bytes > UINT32_MAX || bytes > SIZE_MAX - sizeof(*header) - alignment)
        return 0;
    total = bytes + sizeof(*header) + alignment - 1U;
    {
        zzm_rt_usage_t *usage = usage_for(context.owner, context.memory_class,
                                           context.quota, 1);
        if (!usage || (context.quota != 0U &&
            (bytes > context.quota ||
             usage->used > context.quota - (uint32_t)bytes))) {
            quota_log_once();
            return 0;
        }
    }
    address = memory->alloc(context.owner, context.memory_class, context.quota,
                            (uint32_t)total, (uint32_t)alignment);
    if (!address)
        return 0;
    aligned = ((uintptr_t)address + sizeof(*header) + alignment - 1U) &
              ~(uintptr_t)(alignment - 1U);
    header = (struct zzm_rt_allocation *)(aligned - sizeof(*header));
    header->magic = ZZM_RT_ALLOC_MAGIC;
    header->address = address;
    header->bytes = (uint32_t)bytes;
    header->owner = context.owner;
    header->memory_class = context.memory_class;
    header->quota = context.quota;
    {
        zzm_rt_usage_t *usage = usage_for(context.owner, context.memory_class,
                                           context.quota, 0);
        if (usage)
            usage->used += (uint32_t)bytes;
    }
    return (void *)aligned;
}

void *malloc(size_t bytes) { return memalign(sizeof(void *), bytes); }

void free(void *pointer)
{
    struct zzm_rt_allocation *header;

    if (!pointer)
        return;
    header = (struct zzm_rt_allocation *)pointer - 1;
    if (header->magic != ZZM_RT_ALLOC_MAGIC || !memory || !memory->free)
        return;
    {
        zzm_rt_usage_t *usage = usage_for(header->owner, header->memory_class,
                                           header->quota, 0);
        if (usage && usage->used >= header->bytes)
            usage->used -= header->bytes;
    }
    (void)memory->free(header->address, header->owner);
}

void *calloc(size_t count, size_t bytes)
{
    size_t total;
    void *result;

    if (count != 0U && bytes > SIZE_MAX / count)
        return 0;
    total = count * bytes;
    result = malloc(total);
    if (result)
        memset(result, 0, total);
    return result;
}

void *realloc(void *pointer, size_t bytes)
{
    struct zzm_rt_allocation *header;
    void *replacement;
    size_t copy;

    if (!pointer)
        return malloc(bytes);
    if (bytes == 0U) {
        free(pointer);
        return 0;
    }
    header = (struct zzm_rt_allocation *)pointer - 1;
    if (header->magic != ZZM_RT_ALLOC_MAGIC)
        return 0;
    replacement = memalign(sizeof(void *), bytes);
    if (!replacement)
        return 0;
    copy = header->bytes < bytes ? header->bytes : bytes;
    memcpy(replacement, pointer, copy);
    free(pointer);
    return replacement;
}

int posix_memalign(void **out, size_t alignment, size_t bytes)
{
    void *result;
    if (!out || !is_power_of_two(alignment) || alignment < sizeof(void *))
        return EINVAL;
    result = memalign(alignment, bytes);
    if (!result)
        return ENOMEM;
    *out = result;
    return 0;
}

void *aligned_alloc(size_t alignment, size_t bytes)
{
    if (!is_power_of_two(alignment) || alignment == 0U || bytes % alignment)
        return 0;
    return memalign(alignment, bytes);
}

void *_malloc_r(void *reent, size_t bytes) { (void)reent; return malloc(bytes); }
void _free_r(void *reent, void *pointer) { (void)reent; free(pointer); }
void *_calloc_r(void *reent, size_t count, size_t bytes)
{ (void)reent; return calloc(count, bytes); }
void *_realloc_r(void *reent, void *pointer, size_t bytes)
{ (void)reent; return realloc(pointer, bytes); }
void *_memalign_r(void *reent, size_t alignment, size_t bytes)
{ (void)reent; return memalign(alignment, bytes); }
size_t malloc_usable_size(void *pointer)
{
    struct zzm_rt_allocation *header = pointer ? (struct zzm_rt_allocation *)pointer - 1 : 0;
    return header && header->magic == ZZM_RT_ALLOC_MAGIC ? header->bytes : 0U;
}

#define ZZM_RT_SYSCALL __attribute__((section(".zzm_rt.syscall")))
int _write(int fd, const void *buffer, int length) ZZM_RT_SYSCALL;
int _write_r(void *reent, int fd, const void *buffer, int length) ZZM_RT_SYSCALL;
void _exit(int status) ZZM_RT_SYSCALL;
int _open(const char *path, int flags, int mode) ZZM_RT_SYSCALL;
int _read(int fd, void *buffer, int length) ZZM_RT_SYSCALL;
int _close(int fd) ZZM_RT_SYSCALL;
int _lseek(int fd, int offset, int whence) ZZM_RT_SYSCALL;
int _fstat(int fd, struct stat *status) ZZM_RT_SYSCALL;
int _isatty(int fd) ZZM_RT_SYSCALL;
int _kill(int pid, int signal) ZZM_RT_SYSCALL;
int _getpid(void) ZZM_RT_SYSCALL;

int _write(int fd, const void *buffer, int length)
{ (void)fd; (void)buffer; return length; }
int _write_r(void *reent, int fd, const void *buffer, int length)
{ (void)reent; return _write(fd, buffer, length); }
void exit(int status) { (void)status; zzm_rt_fault("module exit"); for (;;) {} }
void _exit(int status) { (void)status; zzm_rt_fault("module exit"); for (;;) {} }
void abort(void) { zzm_rt_fault("module abort"); for (;;) {} }
void __assert_func(const char *file, int line, const char *func, const char *expr)
{ (void)file; (void)line; (void)func; (void)expr; abort(); }
int _gettimeofday(struct timeval *tv, void *tz)
{ (void)tz; if (!tv || !core || !core->time_us) { errno = ENOSYS; return -1; } uint64_t us = core->time_us(); tv->tv_sec = (long)(us / 1000000U); tv->tv_usec = (long)(us % 1000000U); return 0; }
clock_t clock(void) { return core && core->time_us ? (clock_t)core->time_us() : (clock_t)-1; }
static int no_syscall(void) { errno = ENOSYS; return -1; }
int _open(const char *path, int flags, int mode) { (void)path; (void)flags; (void)mode; return no_syscall(); }
int _read(int fd, void *buffer, int length) { (void)fd; (void)buffer; (void)length; return no_syscall(); }
int _close(int fd) { (void)fd; return no_syscall(); }
int _lseek(int fd, int offset, int whence) { (void)fd; (void)offset; (void)whence; return no_syscall(); }
int _fstat(int fd, struct stat *status) { (void)fd; if (status) memset(status, 0, sizeof(*status)); return no_syscall(); }
int _isatty(int fd) { (void)fd; return 0; }
int _kill(int pid, int signal) { (void)pid; (void)signal; return no_syscall(); }
int _getpid(void) { return -1; }

void *__memcpy_chk(void *destination, const void *source, size_t bytes,
                   size_t destination_size)
{
    if (bytes > destination_size) abort();
    return memcpy(destination, source, bytes);
}

void *__memset_chk(void *destination, int value, size_t bytes,
                   size_t destination_size)
{
    if (bytes > destination_size) abort();
    return memset(destination, value, bytes);
}

long strtol(const char *text, char **end, int base)
{
    unsigned long value = 0U;
    int sign = 1;
    if (!text || (base != 0 && (base < 2 || base > 36))) {
        if (end) *end = (char *)text;
        return 0;
    }
    if (*text == '-') { sign = -1; text++; }
    else if (*text == '+') text++;
    if (base == 0) { base = text[0] == '0' && (text[1] == 'x' || text[1] == 'X') ? 16 : 10; }
    if (base == 16 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;
    while (*text) {
        int digit = *text >= '0' && *text <= '9' ? *text - '0' :
                    *text >= 'a' && *text <= 'z' ? *text - 'a' + 10 :
                    *text >= 'A' && *text <= 'Z' ? *text - 'A' + 10 : base;
        if (digit >= base) break;
        value = value * (unsigned)base + (unsigned)digit;
        text++;
    }
    if (end) *end = (char *)text;
    return sign < 0 ? -(long)value : (long)value;
}

void __cxa_pure_virtual(void) { zzm_rt_fault("pure virtual call"); }
int __cxa_guard_acquire(uint64_t *guard) { if (*guard) return 0; return 1; }
void __cxa_guard_release(uint64_t *guard) { *guard = 1U; }
void __cxa_guard_abort(uint64_t *guard) { *guard = 0U; }
int __cxa_atexit(void (*destructor)(void *), void *object, void *dso)
{ (void)destructor; (void)object; (void)dso; return 0; }
int __aeabi_atexit(void *object, void (*destructor)(void *), void *dso)
{ return __cxa_atexit(destructor, object, dso); }
void *_Znwj(size_t bytes) { return malloc(bytes); }
void *_Znaj(size_t bytes) { return malloc(bytes); }
void _ZdlPv(void *pointer) { free(pointer); }
void _ZdaPv(void *pointer) { free(pointer); }
void _ZdlPvj(void *pointer, size_t bytes) { (void)bytes; free(pointer); }
void _ZdaPvj(void *pointer, size_t bytes) { (void)bytes; free(pointer); }
void *_ZnwjSt11align_val_t(size_t bytes, size_t alignment) __asm__("_ZnwjSt11align_val_t");
void *_ZnwjSt11align_val_t(size_t bytes, size_t alignment) { return memalign(alignment, bytes); }
void *_ZnajSt11align_val_t(size_t bytes, size_t alignment) __asm__("_ZnajSt11align_val_t");
void *_ZnajSt11align_val_t(size_t bytes, size_t alignment) { return memalign(alignment, bytes); }
void _ZdlPvSt11align_val_t(void *pointer, size_t alignment) __asm__("_ZdlPvSt11align_val_t");
void _ZdlPvSt11align_val_t(void *pointer, size_t alignment) { (void)alignment; free(pointer); }
void _ZdaPvSt11align_val_t(void *pointer, size_t alignment) __asm__("_ZdaPvSt11align_val_t");
void _ZdaPvSt11align_val_t(void *pointer, size_t alignment) { (void)alignment; free(pointer); }

typedef void (*zzm_ctor_fn)(void);
extern zzm_ctor_fn __zzm_preinit_array_start[];
extern zzm_ctor_fn __zzm_preinit_array_end[];
extern zzm_ctor_fn __zzm_init_array_start[];
extern zzm_ctor_fn __zzm_init_array_end[];
extern zzm_ctor_fn __zzm_fini_array_start[];
extern zzm_ctor_fn __zzm_fini_array_end[];
extern int zzm_module_init(const zzm_services_t *services);
extern void zzm_module_fini(void) __attribute__((weak));

static void run_initializers(zzm_ctor_fn *begin, zzm_ctor_fn *end)
{
    while (begin != end) {
        if (*begin)
            (*begin)();
        begin++;
    }
}

int zzm_module_entry(const zzm_services_t *services)
{
    run_initializers(__zzm_preinit_array_start, __zzm_preinit_array_end);
    run_initializers(__zzm_init_array_start, __zzm_init_array_end);
    return zzm_module_init(services);
}

void zzm_module_exit(void)
{
    zzm_ctor_fn *fn = __zzm_fini_array_end;
    if (zzm_module_fini)
        zzm_module_fini();
    while (fn != __zzm_fini_array_start) {
        --fn;
        if (*fn)
            (*fn)();
    }
}
