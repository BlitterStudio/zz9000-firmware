/*
 * App entry. r0 is the API table, r1 argc, r2 argv, sp the arena top.
 * No vectors, no BSS clear, no MMU. Return from zz_main routes through
 * libc exit() so newlib flushes buffered stdio (stdout is fully buffered
 * because _isatty reports 0); exit's _exit stub is zz_exit.
 *
 * With ZZAPP_FREESTANDING the image has no libc. The status zz_main returns
 * goes straight to zz_exit. Neither mode calls __libc_init_array, so
 * .init_array constructors never run; only newlib exit runs atexit handlers.
 *
 * SPDX-License-Identifier: MIT
 */
	.syntax unified
	.cpu cortex-a9
	.fpu neon
	.arch armv7-a

	.section .text.zz_start, "ax", %progbits
	.global zz_start
	.type zz_start, %function
zz_start:
	/* 8-byte stack alignment: four registers. */
	push	{r4, r5, r6, lr}
	mov	r4, r1
	mov	r5, r2
	bl	zzapp_crt0_bind
	mov	r0, r4
	mov	r1, r5
	bl	zz_main
#ifdef ZZAPP_FREESTANDING
	bl	zz_exit		/* r0 is the zz_main status */
#else
	bl	exit
#endif
	b	zz_exit		/* unreachable: neither exit nor zz_exit returns */
	.size zz_start, . - zz_start
