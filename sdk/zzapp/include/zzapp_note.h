/*
 * ELF note for a ZZ9000 app image. ZZAPP_NOTE() at file scope emits the
 * loader note: name "ZZAPP", n_type 1, descriptor magic 0x50505A41, abi 0/0,
 * then stack and flags, in section .note.zzapp. A host build (or one compiled
 * with ZZAPP_HOST) expands the macro to nothing.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ZZAPP_NOTE_H
#define ZZAPP_NOTE_H

#if (defined(__arm__) || defined(__ARM_ARCH)) && !defined(ZZAPP_HOST)

#define ZZAPP_NOTE_PASTE_(a, b) a##b
#define ZZAPP_NOTE_PASTE(a, b) ZZAPP_NOTE_PASTE_(a, b)

#define ZZAPP_NOTE_KEEP(id, flags, stack_bytes)                               \
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
            ".long 0\n\t"                                                     \
            ".long %c0\n\t"                                                   \
            ".long %c1\n\t"                                                   \
            ".long 0\n\t"                                                     \
            ".popsection\n\t"                                                 \
            :                                                                 \
            : "i"((unsigned int)(stack_bytes)), "i"((unsigned int)(flags)));  \
    }

#define ZZAPP_NOTE(flags, stack_bytes) \
    ZZAPP_NOTE_KEEP(__COUNTER__, flags, stack_bytes)

#else

#define ZZAPP_NOTE(flags, stack_bytes)

#endif

#endif
