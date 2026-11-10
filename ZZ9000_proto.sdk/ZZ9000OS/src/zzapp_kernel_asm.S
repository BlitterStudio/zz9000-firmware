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
.equ SVC_FRAME_BYTES, 136

core1_vector_svc:
	/* User calls take the compact path.  Save the caller-clobbered state
	 * temporarily on the exception stack before using it to form the frame
	 * on the currently scheduled app's kernel stack. */
	stmdb sp!, {r0-r3, r12, lr}
	mrs r0, spsr
	push {r0}
	sub sp, sp, #12
	add r1, sp, #4
	stmia r1, {sp, lr}^
	ldr r0, [sp, #12]
	and r0, r0, #0x1f
	cmp r0, #0x10
	bne 3f
	ldr r0, =zzapp_kernel_svc_stack_top
	ldr r0, [r0]
	cmp r0, #0
	beq 3f
	/* PMCCNTR stamp after the mode and stack checks, before the compact
	 * frame work. The scheduler publishes this slot before user return. */
	mrc p15, 0, r2, c9, c13, 0
	ldr r1, =zzapp_svc_counter_slot
	ldr r1, [r1]
	ldr r3, =zzapp_svc_entry_stamp
	str r2, [r3, r1, lsl #2]
	sub r0, r0, #SVC_FRAME_BYTES
	str r0, [sp]
	add r1, sp, #4
	ldmia r1, {r2, r3}
	stmia r0, {r2, r3}
	/* SPSR into the frame: the fast exit returns to the app with it. */
	ldr r1, [sp, #12]
	str r1, [r0, #8]
	add r1, r0, #12
	ldr r2, [sp, #32]
	/* API_SLOT(time_us) has no VFP arguments or result. It is the
	 * empty-call timing path, so avoid spilling s0-s15 there. */
	cmp r2, #3
	beq 4f
	vstmia r1, {d0-d7}
4:
	add r1, r0, #76
	ldr r2, [sp, #16]
	str r2, [r1]
	ldr r2, [sp, #20]
	str r2, [r1, #4]
	ldr r2, [sp, #24]
	str r2, [r1, #8]
	ldr r2, [sp, #28]
	str r2, [r1, #12]
	ldr r2, [sp, #32]
	str r2, [r1, #48]
	ldr r2, [sp, #36]
	str r2, [r0, #128]
	/* Release the shared SVC stack before the body can be preempted or
	 * block: another thread's SVC entry must never find this thread's
	 * scratch words on it.  Everything needed later is in the frame. */
	add sp, sp, #40
	cps #0x1f
	mov sp, r0
	cpsie i
	bl zzapp_svc_dispatch
	cpsid i
	bl zzapp_kernel_svc_can_return
	/* Stamp as late as possible on the fast path. Preserve the return
	 * decision while the C helper accumulates the plain per-slot stores. */
	push {r0, r1}	/* r1 keeps the stack 8-byte aligned for the call */
	mrc p15, 0, r0, c9, c13, 0
	ldr r1, =zzapp_svc_eret_stamp
	ldr r2, =zzapp_svc_counter_slot
	ldr r2, [r2]
	str r0, [r1, r2, lsl #2]
	bl zzapp_svc_counters_eret
	pop {r0, r1}
	/* SYS SP is the frame again (both calls are balanced). */
	mov r2, sp
	cps #0x13
	cmp r0, #0
	beq 1f
	/* The bl calls above ran in SYS mode and used the user bank's SP and
	 * LR; give the app back its own before returning. */
	ldmia r2, {sp, lr}^
	nop
	ldr r0, [r2, #76]
	ldr r1, [r2, #80]
	ldr r3, [r2, #8]
	ldr lr, [r2, #128]
	msr spsr_cxsf, r3
	subs pc, lr, #0
1:
	/* Blocking, stopping and service handover are uncommon.  Materialize the
	 * complete scheduler frame only here, then use the established switch
	 * path.  r12 is caller-clobbered for the app, so it carries the compact
	 * frame across save_frame. */
	mov r12, r2
	save_frame
	add r5, r12, #12
	vldmia r5, {d0-d7}
	add r5, sp, #140
	vstmia r5, {d0-d7}
	ldr r5, [r12]
	str r5, [sp]
	ldr r5, [r12, #4]
	str r5, [sp, #4]
	ldr r5, [r12, #8]
	str r5, [sp, #8]
	ldr r5, [r12, #76]
	str r5, [sp, #272]
	ldr r5, [r12, #80]
	str r5, [sp, #276]
	ldr r5, [r12, #84]
	str r5, [sp, #280]
	ldr r5, [r12, #88]
	str r5, [sp, #284]
	ldr r5, [r12, #124]
	str r5, [sp, #320]
	ldr r5, [r12, #128]
	str r5, [sp, #324]
	select_frame 2, 0
	restore_frame 0
3:
	/* A privileged SVC remains the scheduler's voluntary switch.  Kernel
	 * code issues it without declaring clobbers, so restore r0-r3 (used
	 * above) before saving its frame, and release the scratch words. */
	add r1, sp, #16
	ldmia r1, {r0-r3}
	add sp, sp, #40
	save_frame
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
	mov r12, #svc_number
	svc #0
	bx lr
.set svc_number, svc_number + 1
.endr
.set svc_number, 128
.rept 128
	movw r12, #svc_number
	svc #0
	bx lr
.set svc_number, svc_number + 1
.endr
.space 1024
