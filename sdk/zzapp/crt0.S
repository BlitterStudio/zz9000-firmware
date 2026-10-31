/*
 * App entry. r0 is the API table, r1 argc, r2 argv, sp the arena top.
 * No vectors, no BSS clear, no MMU. Return from zz_main is zz_exit.
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
	b	zz_exit
	.size zz_start, . - zz_start
