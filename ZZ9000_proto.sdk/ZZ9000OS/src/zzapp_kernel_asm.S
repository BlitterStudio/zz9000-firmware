/*
 * Core-1 exception vectors and one-frame VFP save.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

.syntax unified
.arm
.section .zzapp_vectors, "ax", %progbits
.align 5
.global zzapp_core1_vectors
.type zzapp_core1_vectors, %function

zzapp_core1_vectors:
	b core1_vector_reset
	b core1_vector_undef
	b core1_vector_svc
	b core1_vector_pabort
	b core1_vector_dabort
	b core1_vector_reserved
	b core1_vector_irq
	b core1_vector_fiq

.macro save_frame
	stmdb sp!, {r0-r12, lr}
	vpush {d0-d15}
	vpush {d16-d31}
	vmrs r0, fpscr
	push {r0}
.endm

.macro restore_frame, adjust
	pop {r0}
	vmsr fpscr, r0
	vpop {d16-d31}
	vpop {d0-d15}
	ldmia sp!, {r0-r12, lr}
	subs pc, lr, #\adjust
.endm

core1_vector_reset:
	b core1_entry

core1_vector_irq:
	save_frame
	bl zzapp_kernel_irq_dispatch
	restore_frame 4

core1_vector_undef:
	save_frame
	ldr r0, =UndefinedExceptionAddr
	sub r1, lr, #4
	str r1, [r0]
	bl arm_exception_handler_illinst
	restore_frame 0

core1_vector_svc:
	save_frame
	bl SWInterrupt
	restore_frame 0

core1_vector_pabort:
	save_frame
	ldr r0, =PrefetchAbortAddr
	sub r1, lr, #4
	str r1, [r0]
	bl arm_exception_handler_id_prefetch_abort
	restore_frame 4

core1_vector_dabort:
	dsb
	save_frame
	ldr r0, =DataAbortAddr
	sub r1, lr, #8
	str r1, [r0]
	bl arm_exception_handler_id_data_abort
	restore_frame 8

core1_vector_reserved:
	b core1_vector_reserved

core1_vector_fiq:
	save_frame
	bl FIQInterrupt
	restore_frame 4
