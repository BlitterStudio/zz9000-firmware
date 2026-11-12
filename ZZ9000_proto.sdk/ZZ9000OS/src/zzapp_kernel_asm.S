/*
 * Core-1 exception vectors and one-frame VFP save.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZAPP_PERF_COUNTERS
#define ZZAPP_PERF_COUNTERS 1
#endif

#ifndef ZZAPP_SVC_PROBES
#define ZZAPP_SVC_PROBES 0
#endif

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

.macro perf_switch_entry
#if ZZAPP_PERF_COUNTERS
	stmdb sp!, {r0, r1}
	mrc p15, 0, r0, c9, c13, 0
	ldr r1, =zzapp_kernel_switch_entry_stamp
	str r0, [r1]
	ldmia sp!, {r0, r1}
#endif
.endm

.macro perf_switch_exit
#if ZZAPP_PERF_COUNTERS
	stmdb sp!, {r0, r1}
	mrc p15, 0, r0, c9, c13, 0
	ldr r1, =zzapp_kernel_switch_exit_stamp
	str r0, [r1]
	ldr r1, =zzapp_kernel_switch_entry_stamp
	ldr r0, [r1]
	ldr r1, =zzapp_kernel_switch_completed_entry_stamp
	str r0, [r1]
	mov r0, #1
	ldr r1, =zzapp_kernel_switch_complete
	str r0, [r1]
	ldmia sp!, {r0, r1}
#endif
.endm

.macro restore_switch_frame, adjust
	ldmia sp, {sp, lr}^
	nop
	add sp, sp, #8
	pop {r0}
	msr spsr_cxsf, r0
	vpop {d16-d31}
	vpop {d0-d15}
	pop {r0}
	vmsr fpscr, r0
	ldmia sp!, {r0-r12, lr}
	perf_switch_exit
	subs pc, lr, #\adjust
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
	perf_switch_entry
	save_frame
	bl zzapp_kernel_irq_dispatch
	select_irq_frame 4
	restore_switch_frame 4

core1_vector_undef:
	perf_switch_entry
	save_frame
	ldr r0, =UndefinedExceptionAddr
	sub r1, lr, #4
	str r1, [r0]
	bl arm_exception_handler_illinst
	select_frame 1, 0
	restore_switch_frame 0

/* Bytes in one save_frame: SYS SP/LR, SPSR, 32 D registers, FPSCR,
 * r0-r12 and LR.  zzapp_kernel.h asserts the C struct has this size. */
.equ FRAME_BYTES, 328
.equ SVC_FRAME_BYTES, 136

core1_vector_svc:
	/* Keep only caller-clobbered state on the SVC stack while a compact
	 * frame is built directly on the current app's kernel stack. */
	stmdb sp!, {r0-r3, r12, lr}
#if ZZAPP_SVC_PROBES
	/* P0 only for SVCs from user mode: privileged callers (sleep and
	 * scheduler hand-offs) leave r12 undefined. r0 is saved above and
	 * reloaded below; the flags are recomputed. */
	cmp r12, #0x100
	bne 9f
	mrs r0, spsr
	and r0, r0, #0x1f
	cmp r0, #0x10
	beq .Lsvc_probe_p0_return
9:
#endif
	mrs r0, spsr
	push {r0}
	ldr r0, [sp]
	and r0, r0, #0x1f
	cmp r0, #0x10
	bne 3f
	ldr r0, =zzapp_kernel_svc_stack_top
	ldr r0, [r0]
	cmp r0, #0
	beq 3f
#if ZZAPP_SVC_PROBES
	movw r1, #0x101
	cmp r12, r1
	beq .Lsvc_probe_early_return
#endif
#if ZZAPP_PERF_COUNTERS
	/* PMCCNTR stamp after the mode and stack checks, before the compact
	 * frame work. The scheduler publishes this slot before user return. */
	mrc p15, 0, r2, c9, c13, 0
	ldr r1, =zzapp_svc_counter_slot
	ldr r1, [r1]
	ldr r3, =zzapp_svc_entry_stamp
	str r2, [r3, r1, lsl #2]
#endif
	sub r0, r0, #SVC_FRAME_BYTES
	/* Store the user-bank stack and link directly in the compact frame. */
	stmia r0, {sp, lr}^
	ldr r1, [sp]
	str r1, [r0, #8]
	add r1, r0, #12
	ldr r2, [sp, #20]
	/* API_SLOT(should_stop) and API_SLOT(time_us) have no VFP arguments
	 * or results, so avoid spilling s0-s15 on the empty-call paths. */
	cmp r2, #2
	beq 4f
	cmp r2, #3
	beq 4f
	vstmia r1, {d0-d7}
4:
	add r1, r0, #76
	ldr r2, [sp, #4]
	str r2, [r1]
	ldr r2, [sp, #8]
	str r2, [r1, #4]
	ldr r2, [sp, #12]
	str r2, [r1, #8]
	ldr r2, [sp, #16]
	str r2, [r1, #12]
	ldr r2, [sp, #20]
	str r2, [r1, #48]
	ldr r2, [sp, #24]
	str r2, [r0, #128]
	/* Release the shared SVC stack before the body can be preempted or
	 * block: another thread's SVC entry must never find this thread's
	 * scratch words on it.  Everything needed later is in the frame. */
	add sp, sp, #28
	cps #0x1f
	mov sp, r0
	cpsie i
#if ZZAPP_SVC_PROBES
	ldr r2, [sp, #124]
	movw r1, #0x102
	cmp r2, r1
	bne 6f
	mov r1, #0
	str r1, [sp, #76]
#if ZZAPP_PERF_COUNTERS
	ldr r2, =zzapp_svc_counter_slot
	ldr r2, [r2]
	ldr r3, =zzapp_svc_counter_class
	ldrb r1, [r3, r2]
	add r3, r2, r2, lsl #1
	add r3, r3, r2, lsl #3
	lsl r3, r3, #4
	ldr r12, =zzapp_svc_counters
	add r12, r12, r3
	cmp r1, #0
	addne r12, r12, #88
	mov r1, #0
	strb r1, [r12, #76]
#endif
	b .Lsvc_probe_return
6:
#endif
	bl zzapp_svc_dispatch
#if ZZAPP_SVC_PROBES
.Lsvc_probe_return:
#endif
	cpsid i
	bl zzapp_kernel_svc_can_return
#if ZZAPP_PERF_COUNTERS
	/* Fold the final PMU stage into its counter.  This preserves the return
	 * decision in r0 and avoids a C helper and an aligned call frame. */
	ldr r2, =zzapp_svc_counter_slot
	ldr r2, [r2]
	ldr r3, =zzapp_svc_counter_class
	ldrb r1, [r3, r2]
	add r3, r2, r2, lsl #1
	add r3, r3, r2, lsl #3
	lsl r3, r3, #4
	ldr r12, =zzapp_svc_counters
	add r12, r12, r3
	cmp r1, #0
	addne r12, r12, #88
	ldrb r3, [r12, #76]
	cmp r3, #0
	beq 5f
	mrc p15, 0, r1, c9, c13, 0
	ldr r3, [r12, #72]
	sub r1, r1, r3
	ldr r3, [r12, #60]
	add r3, r3, #1
	str r3, [r12, #60]
	ldr r3, [r12, #48]
	adds r3, r3, r1
	str r3, [r12, #48]
	ldr r3, [r12, #52]
	adc r3, r3, #0
	str r3, [r12, #52]
	ldr r3, [r12, #56]
	cmp r1, r3
	strhi r1, [r12, #56]
5:
#endif
	/* SYS SP is the frame again. */
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
	perf_switch_entry
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
	restore_switch_frame 0
3:
	/* A privileged SVC remains the scheduler's voluntary switch.  Kernel
	 * code issues it without declaring clobbers, so restore r0-r3 (used
	 * above) before saving its frame, and release the scratch words. */
	add r1, sp, #4
	ldmia r1, {r0-r3}
	add sp, sp, #28
	perf_switch_entry
	save_frame
	select_frame 2, 0
	restore_switch_frame 0
#if ZZAPP_SVC_PROBES
.Lsvc_probe_p0_return:
	/* P0 has only the six scratch words; SPSR is untouched in its bank. */
	ldmia sp!, {r0-r3, r12, lr}
	mov r0, #0
	subs pc, lr, #0

.Lsvc_probe_early_return:
	/* The scratch frame holds the original user r1-r3/r12/lr and SPSR.
	 * r0 is the documented zero probe result, so it is intentionally not
	 * restored.  P0/P1 have no compact frame to hand to the scheduler. */
	ldr r0, [sp]
	msr spsr_cxsf, r0
	add sp, sp, #4
	ldmia sp!, {r0-r3, r12, lr}
	mov r0, #0
	subs pc, lr, #0
#endif

core1_vector_pabort:
	perf_switch_entry
	save_frame
	ldr r0, =PrefetchAbortAddr
	sub r1, lr, #4
	str r1, [r0]
	bl arm_exception_handler_id_prefetch_abort
	select_frame 1, 4
	restore_switch_frame 4

core1_vector_dabort:
	dsb
	perf_switch_entry
	save_frame
	ldr r0, =DataAbortAddr
	sub r1, lr, #8
	str r1, [r0]
	bl arm_exception_handler_id_data_abort
	select_frame 1, 8
	restore_switch_frame 8

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
