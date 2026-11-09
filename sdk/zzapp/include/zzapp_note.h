/*
 * ELF note for a ZZ9000 app image. ZZAPP_NOTE() at file scope emits the
 * loader note: name "ZZAPP", n_type 1, descriptor magic 0x50505A41, abi
 * major/minor, then stack, flags and an optional minimum heap in MiB, in
 * section .note.zzapp. A host build (or one compiled with ZZAPP_HOST)
 * expands the macro to nothing.
 *
 * ZZAPP_NOTE() keeps abi 0/0 and adds the dynamic-heap flag, so the app
 * still loads on API 0.2 firmware (with the fixed heap there).
 * ZZAPP_NOTE_MIN_HEAP() needs API 0.3 firmware and declares abi 0/3.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ZZAPP_NOTE_H
#define ZZAPP_NOTE_H

#if (defined(__arm__) || defined(__ARM_ARCH)) && !defined(ZZAPP_HOST)

#define ZZAPP_NOTE_PASTE_(a, b) a##b
#define ZZAPP_NOTE_PASTE(a, b) ZZAPP_NOTE_PASTE_(a, b)

/* Same value as ZZAPP_NOTE_FLAG_DYNAMIC_HEAP in zzapp.h. */
#define ZZAPP_NOTE_DYNAMIC_HEAP_ 8u

#define ZZAPP_NOTE_KEEP(id, minor, flags, stack_bytes, min_heap_mib)          \
    static void ZZAPP_NOTE_PASTE(zzapp_note_keep_, id)(void)                  \
        __attribute__((used, noinline));                                      \
    static void ZZAPP_NOTE_PASTE(zzapp_note_keep_, id)(void)                  \
    {                                                                         \
        __asm__ __volatile__(                                                 \
            ".pushsection .note.zzapp, \"a\", %%note\n\t"                     \
            ".balign 4\n\t"                                                   \
            ".long 6\n\t"                                                     \
            ".long 24\n\t"                                                    \
            ".long 1\n\t"                                                     \
            ".asciz \"ZZAPP\"\n\t"                                            \
            ".balign 4\n\t"                                                   \
            ".long 0x50505A41\n\t"                                            \
            ".long 0\n\t"                                                     \
            ".long %c3\n\t"                                                   \
            ".long %c0\n\t"                                                   \
            ".long %c1\n\t"                                                   \
            ".long %c2\n\t"                                                   \
            ".popsection\n\t"                                                 \
            :                                                                 \
            : "i"((unsigned int)(stack_bytes)),                               \
              "i"((unsigned int)(flags) | ZZAPP_NOTE_DYNAMIC_HEAP_),           \
              "i"((unsigned int)(min_heap_mib)), "i"((unsigned int)(minor)));  \
    }

#define ZZAPP_NOTE_MIN_HEAP(flags, stack_bytes, min_heap_mib) \
    ZZAPP_NOTE_KEEP(__COUNTER__, 3u, flags, stack_bytes, min_heap_mib)

#define ZZAPP_NOTE(flags, stack_bytes) \
    ZZAPP_NOTE_KEEP(__COUNTER__, 0u, flags, stack_bytes, 0u)

#else

#define ZZAPP_NOTE_MIN_HEAP(flags, stack_bytes, min_heap_mib)
#define ZZAPP_NOTE(flags, stack_bytes)

#endif

#endif
