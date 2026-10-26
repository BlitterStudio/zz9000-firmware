/* ARM ELF fixtures for util/check_zzm_conformance.sh. */
/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <stddef.h>
#include <stdint.h>

const uint32_t zzm_rt_identity = 0x5a5a4d32U;
const uint32_t zzm_module_descriptor = 1U;

void *malloc(size_t bytes) { (void)bytes; return 0; }
void free(void *pointer) { (void)pointer; }
void *calloc(size_t count, size_t bytes) { (void)count; (void)bytes; return 0; }
void *realloc(void *pointer, size_t bytes) { (void)pointer; (void)bytes; return 0; }
void *memalign(size_t alignment, size_t bytes) { (void)alignment; (void)bytes; return 0; }
int posix_memalign(void **out, size_t alignment, size_t bytes)
{ (void)out; (void)alignment; (void)bytes; return 0; }
void *aligned_alloc(size_t alignment, size_t bytes)
{ (void)alignment; (void)bytes; return 0; }

#ifndef UNRESOLVED_ALLOCATOR
void *_malloc_r(void *reent, size_t bytes) { (void)reent; (void)bytes; return 0; }
#else
extern void *_malloc_r(void *reent, size_t bytes);
static void *unresolved_allocator_reference(void) { return _malloc_r(0, 0U); }
#endif
void _free_r(void *reent, void *pointer) { (void)reent; (void)pointer; }
void *_calloc_r(void *reent, size_t count, size_t bytes)
{ (void)reent; (void)count; (void)bytes; return 0; }
void *_realloc_r(void *reent, void *pointer, size_t bytes)
{ (void)reent; (void)pointer; (void)bytes; return 0; }
void *_memalign_r(void *reent, size_t alignment, size_t bytes)
{ (void)reent; (void)alignment; (void)bytes; return 0; }

#ifdef LOCAL_HEAP
void *_sbrk(int increment) { (void)increment; return 0; }
#endif

#ifdef STUB_SYSCALL
int _open(const char *path, int flags, int mode)
{ (void)path; (void)flags; (void)mode; return -1; }
#endif

#ifdef UNDEFINED_SYMBOL
extern void fixture_undefined_symbol(void);
#endif

#ifdef UNWIND_THROW
void __cxa_throw(void) {}
#endif

#ifdef OUTSIDE_SLOT
volatile const uint8_t outside_slot_data[16]
    __attribute__((section(".outside"))) = { 1U };
#endif

void zzm_module_entry(void)
{
#ifdef UNRESOLVED_ALLOCATOR
    (void)unresolved_allocator_reference();
#endif
#ifdef UNDEFINED_SYMBOL
    fixture_undefined_symbol();
#endif
#ifdef OUTSIDE_SLOT
    (void)outside_slot_data[0];
#endif
}

int zzm_module_init(const void *services)
{
    (void)services;
    return 0;
}

void zzm_module_exit(void)
{
}
