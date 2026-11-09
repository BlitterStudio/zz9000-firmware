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

/*
 * One complete switch frame, lowest address first:
 * SYS r13/r14, SPSR, d16-d31, d0-d15, FPSCR, r0-r12, exception LR.
 * zzapp_kernel_switch_frame() records that layout in the selected thread
 * before it returns the next frame; restore_frame changes exception SP to it.
 */
.macro save_frame
	stmdb sp!, {r0-r12, lr}
	vmrs r0, fpscr
	push {r0}
	vpush {d0-d15}
	vpush {d16-d31}
	mrs r0, spsr
	push {r0}
	/* Writeback with the user-bank form is UNPREDICTABLE: adjust SP first. */
	sub sp, sp, #8
	stmia sp, {sp, lr}^
.endm

.macro select_frame, source, adjust
	mov r4, sp
	mov r0, sp
	mov r1, #\source
	mov r2, #\adjust
	bl zzapp_kernel_arch_dispatch
	cmp r0, #0
	moveq r0, r4
	mov sp, r0
.endm

.macro select_irq_frame, adjust
	mov r4, sp
	mov r1, r0
	mov r0, sp
	mov r2, #\adjust
	bl zzapp_kernel_arch_dispatch
	cmp r0, #0
	moveq r0, r4
	mov sp, r0
.endm

.macro restore_frame, adjust
	ldmia sp, {sp, lr}^
	/* No banked-register access directly after a user-bank LDM. */
	nop
	add sp, sp, #8
	pop {r0}
	msr spsr_cxsf, r0
	vpop {d16-d31}
	vpop {d0-d15}
	pop {r0}
	vmsr fpscr, r0
	ldmia sp!, {r0-r12, lr}
	subs pc, lr, #\adjust
.endm

core1_vector_reset:
	b core1_entry

core1_vector_irq:
	save_frame
	bl zzapp_kernel_irq_dispatch
	select_irq_frame 4
	restore_frame 4

core1_vector_undef:
	save_frame
	ldr r0, =UndefinedExceptionAddr
	sub r1, lr, #4
	str r1, [r0]
	bl arm_exception_handler_illinst
	select_frame 1, 0
	restore_frame 0

/* Bytes in one save_frame: SYS SP/LR, SPSR, 32 D registers, FPSCR,
 * r0-r12 and LR.  zzapp_kernel.h asserts the C struct has this size. */
.equ FRAME_BYTES, 328

core1_vector_svc:
	save_frame
	/* Only a veneer call from user mode is an API call.  A trap from a
	 * privileged mode is a voluntary switch by kernel code. */
	ldr r0, [sp, #8]
	and r0, r0, #0x1f
	cmp r0, #0x10
	bne 1f
	mov r4, sp
	bl zzapp_kernel_current_thread
	bl scheduler_core1_app_stack_top
	cmp r0, #0
	beq 1f
	/* Move the frame to the top of the caller's kernel stack and release
	 * the shared SVC stack: a call that blocks or is preempted must not
	 * leave its frame where the next thread's SVC entry would land. */
	sub r5, r0, #FRAME_BYTES
	mov r0, r5
	mov r1, r4
	mov r2, #FRAME_BYTES
	bl memcpy
	add sp, r4, #FRAME_BYTES
	cps #0x1f
	mov sp, r5
	cpsie i
	mov r0, r5
	bl zzapp_svc_dispatch
	cpsid i
	cps #0x13
	sub sp, sp, #FRAME_BYTES
	mov r4, sp
	mov r0, r4
	mov r1, r5
	mov r2, #FRAME_BYTES
	bl memcpy
	/* An ordinary call returns to its caller; switch only if the call
	 * left the thread blocked, sleeping or stopped. */
	mov r0, r4
	bl zzapp_kernel_svc_return
	mov sp, r0
	restore_frame 0
1:
	select_frame 2, 0
	restore_frame 0

core1_vector_pabort:
	save_frame
	ldr r0, =PrefetchAbortAddr
	sub r1, lr, #4
	str r1, [r0]
	bl arm_exception_handler_id_prefetch_abort
	select_frame 1, 4
	restore_frame 4

core1_vector_dabort:
	dsb
	save_frame
	ldr r0, =DataAbortAddr
	sub r1, lr, #8
	str r1, [r0]
	bl arm_exception_handler_id_data_abort
	select_frame 1, 8
	restore_frame 8

core1_vector_reserved:
	b core1_vector_reserved

core1_vector_fiq:
	save_frame
	bl FIQInterrupt
	restore_frame 4

.section .zzapp_svc_veneers, "ax", %progbits
.align 12
.global zzapp_svc_veneer_page
.type zzapp_svc_veneer_page, %function
zzapp_svc_veneer_page:
.set svc_number, 0
.rept 128
	svc #svc_number
	bx lr
.set svc_number, svc_number + 1
.endr
.set svc_number, 128
.rept 128
	svc #svc_number
	bx lr
.set svc_number, svc_number + 1
.endr
.space 2048
