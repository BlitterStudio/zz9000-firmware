#include <stdint.h>
#include <stdio.h>
#include "xil_cache.h"
#include "xil_exception.h"
#include "xil_io.h"
#include "xil_misc_psreset_api.h"
#include "core2.h"
#include "sleep.h"
#include "scheduler.h"
#include "memorymap.h"

#define A9_CPU_RST_CTRL		(XSLCR_BASEADDR + 0x244)
#define A9_RST1_MASK 		0x00000002
#define A9_CLKSTOP1_MASK	0x00000020

#define XSLCR_LOCK_ADDR		(XSLCR_BASEADDR + 0x4)
#define XSLCR_LOCK_CODE		0x0000767B

uint16_t arm_app_output_event_serial = 0;
uint16_t arm_app_output_event_code = 0;
char arm_app_output_event_ack = 0;
uint16_t arm_app_output_events_blocking = 0;
uint16_t arm_app_output_putchar_to_events = 0;
uint16_t arm_app_input_event_serial = 0;
uint16_t arm_app_input_event_code = 0;
char arm_app_input_event_ack = 0;

uint32_t arm_app_output_events_timeout = 100000;

volatile struct ZZ9K_ENV arm_run_env;

volatile struct ZZ9K_ENV* arm_app_get_run_env() {
	return &arm_run_env;
}

void arm_app_put_event_code(uint16_t code) {
	arm_app_output_event_code = code;
	arm_app_output_event_ack = 0;
	arm_app_output_event_serial++;
}

char arm_app_output_event_acked() {
	return arm_app_output_event_ack;
}

void arm_app_set_output_events_blocking(char blocking) {
	arm_app_output_events_blocking = blocking;
}

void arm_app_set_output_putchar_to_events(char putchar_enabled) {
	arm_app_output_putchar_to_events = putchar_enabled;
}

uint16_t arm_app_get_event_serial() {
	return arm_app_input_event_serial;
}

uint16_t arm_app_get_event_code() {
	arm_app_input_event_ack = 1;
	return arm_app_input_event_code;
}

int __attribute__ ((visibility ("default"))) _putchar(char c) {
	if (arm_app_output_putchar_to_events) {
		if (arm_app_output_events_blocking) {
			for (uint32_t i = 0; i < arm_app_output_events_timeout; i++) {
				usleep(1);
				if (arm_app_output_event_ack)
					break;
			}
		}
		arm_app_put_event_code(c);
	}
	return putchar(c);
}

//void DataAbort_InterruptHandler(void *InstancePtr);

volatile void (*core1_trampoline)(volatile struct ZZ9K_ENV* env);
volatile int core2_execute = 0;

void core1_loop();  /* forward decl: core1_entry branches into this C body */
void core1_entry(void);  /* naked reset stub: 0xFFFFFFF0 points here; sets SP then branches to core1_loop */

struct core_fault_slot core1_fault __attribute__((aligned(32))) = { CORE_FAULT_NONE, {0} };

static void record_fault(uint32_t code, const char *name)
{
	taskq_shared_t *sh = scheduler_shared();
	int slot = sh->core1_current_slot;

	core1_fault.code = code;
	Xil_DCacheFlushRange((INTPTR)&core1_fault, sizeof(core1_fault));
	printf("%s: arm_exception_handler()!\n", name);

	if (slot >= 0) {
		/*
		 * Core 1 faulted while running a dual-core scheduler task. Don't hang
		 * the card: mark the in-flight slot FAILED so core 0 posts an error
		 * completion (the Amiga falls back to software crypto), record the
		 * fault code, and request a cold restart. core 0's harvest maps every
		 * FAILED task to SDK_STATUS_INTERNAL_ERROR, so the status passed here
		 * is unused. Then park on WFE until core 0 resets us back into
		 * core1_loop (core1_cold_restart).
		 */
		taskq_fail(&sh->queue, slot, 0);
		sh->core1_current_slot = -1;
		sh->core1_fault_code = code;
		sh->core1_restart_request = 1;
		Xil_DCacheFlushRange((INTPTR)sh, sizeof(*sh));
		__asm__ __volatile__("dsb" ::: "memory");
		for (;;) {
			__asm__ __volatile__("wfe" ::: "memory");
		}
	}

	/*
	 * Legacy surface-coprocessor path (no scheduler task in flight): hang until
	 * core 0 notices core1_fault.code and resets the core.
	 */
	while (1) {
	}
}

/*
 * Cold-restart core 1 by re-running the CPU1 reset sequence (XAPP1079), so it
 * re-enters core1_entry from its reset vector. Used by the dual-core scheduler
 * (scheduler_core0_poll) to recover a worker that faulted and parked. 0xFFFFFFF0
 * is re-pointed at core1_entry first in case the OCM word was disturbed.
 */
void core1_cold_restart(void)
{
	volatile uint32_t *core1_addr = (volatile uint32_t *) 0xFFFFFFF0;
	uint32_t RegVal;

	*core1_addr = (uint32_t) core1_entry;
	Xil_DCacheFlush();  /* the reset vector write must reach OCM before CPU1 reads it */

	Xil_Out32(XSLCR_UNLOCK_ADDR, XSLCR_UNLOCK_CODE);
	RegVal = Xil_In32(A9_CPU_RST_CTRL);
	RegVal |= A9_RST1_MASK;
	Xil_Out32(A9_CPU_RST_CTRL, RegVal);
	RegVal |= A9_CLKSTOP1_MASK;
	Xil_Out32(A9_CPU_RST_CTRL, RegVal);
	RegVal &= ~A9_RST1_MASK;
	Xil_Out32(A9_CPU_RST_CTRL, RegVal);
	RegVal &= ~A9_CLKSTOP1_MASK;
	Xil_Out32(A9_CPU_RST_CTRL, RegVal);
	Xil_Out32(XSLCR_LOCK_ADDR, XSLCR_LOCK_CODE);

	dmb();
	dsb();
	isb();
	asm("sev");
}

/*
 * SCHED_CORE1_TRACE (opt-in, off by default): raw UART1 (STDOUT_BASEADDRESS
 * 0xE0001000) byte output for core-1 bring-up diagnostics. Deliberately uses NO
 * libc, NO malloc, NO MMU and a trivial leaf-function stack frame, so it is
 * valid on core 1 before the MMU/D-cache are up and regardless of any heap/stack
 * corruption. SR @ 0x2C (TXFULL = 0x10), TX FIFO @ 0x30. Define SCHED_CORE1_TRACE
 * to re-enable the staged [c1] R0..R3 markers when diagnosing bring-up; the
 * production boot signal is the single "[sched] core 1 worker online" line.
 */
#ifdef SCHED_CORE1_TRACE
static void c1_trace(const char *s)
{
	volatile uint32_t *sr   = (volatile uint32_t *)(0xE0001000u + 0x2Cu);
	volatile uint32_t *fifo = (volatile uint32_t *)(0xE0001000u + 0x30u);
	while (*s) {
		while (*sr & 0x10u) { }          /* wait while TX FIFO full */
		*fifo = (uint32_t)(unsigned char)*s++;
	}
}
#define C1_TRACE(s) c1_trace(s)
#else
#define C1_TRACE(s) ((void)0)
#endif

/*
 * Core-1 reset entry. The CPU1 reset vector (0xFFFFFFF0) points here. This must
 * be NAKED: on Cortex-A9 reset the stack pointer is UNPREDICTABLE, so any
 * compiler-generated prologue would push registers to a garbage SP and fault
 * before a single instruction of C ran -- which is exactly what killed core 1
 * (no markers at all, even the raw-UART one). We set SP to the reserved core-1
 * stack in asm FIRST, with nothing on the stack yet, then branch into the C
 * body. `b` (not `bl`): core1_loop never returns.
 *
 * Basic asm only (naked functions do not reliably support extended asm); the
 * stack-top constant is single-sourced from memorymap.h via stringification.
 * SDK_CORE1_STACK_TOP must be a valid ARM `mov` immediate for this one insn.
 */
#define CORE1_STR2(x) #x
#define CORE1_STR(x)  CORE1_STR2(x)
__attribute__((naked, used)) void core1_entry(void)
{
	__asm__ volatile(
		"mov	sp, #" CORE1_STR(SDK_CORE1_STACK_TOP) "\n\t"
		"b	core1_loop\n\t");
}

#pragma GCC push_options
#pragma GCC optimize ("O1")
// core1_loop is executed on core1 (vs core0), entered via core1_entry
void core1_loop() {
	C1_TRACE("[c1] R0 entry (stub set SP)\r\n");  /* proof: CPU1 reached the C body */
	asm("mov	r0, r0");
	asm("mrc	p15, 0, r1, c1, c0, 2");
	/* read cp access control register (CACR) into r1 */
	asm("orr	r1, r1, #(0xf << 20)");
	/* enable full access for p10 & p11 */
	asm("mcr	p15, 0, r1, c1, c0, 2");
	/* write back into CACR */

	// enable FPU
	asm("fmrx	r1, FPEXC");
	/* read the exception register */
	asm("orr	r1,r1, #0x40000000");
	/* set VFP enable bit, leave the others in orig state */
	asm("fmxr	FPEXC, r1");
	/* write back the exception register */

	// enable flow prediction
	asm("mrc	p15,0,r0,c1,c0,0");
	/* flow prediction enable */
	asm("orr	r0, r0, #(0x01 << 11)");
	/* #0x8000 */
	asm("mcr	p15,0,r0,c1,c0,0");

	asm("mrc	p15,0,r0,c1,c0,1");
	/* read Auxiliary Control Register */
	asm("orr	r0, r0, #(0x1 << 2)");
	/* enable Dside prefetch */
	asm("orr	r0, r0, #(0x1 << 1)");
	/* enable L2 Prefetch hint */
	asm("mcr	p15,0,r0,c1,c0,1");
	/* write Auxiliary Control Register */

	// SP was established by the core1_entry reset stub (SDK_CORE1_STACK_TOP),
	// before any C ran -- do NOT reset it here.

	volatile uint32_t* addr = 0;
	addr[0] = 0xe3e0000f; // mvn	r0, #15  -- loads 0xfffffff0
	addr[1] = 0xe590f000; // ldr	pc, [r0] -- jumps to the address in that address

	C1_TRACE("[c1] R1 loop-entry (mmu off)\r\n");  /* core 1 is executing core1_loop */

	// Bring core 1 up to MMU + D-cache + SMP so the dual-core task scheduler's
	// cross-core LDREX/STREX and SCU coherency work (core 0 marked the
	// task-queue region shareable at boot, before this core started).
	scheduler_coherency_init_core1();
	C1_TRACE("[c1] R2 coherency-ok (mmu on)\r\n");  /* survived MMU/cache/SMP bring-up */

#ifdef SCHED_STRESS_TEST
	scheduler_stress_core1();  // Phase 0 two-core coherency torture; never returns
#endif

	// Core 1 runs the dual-core task-scheduler worker and never returns. It
	// executes work offloaded from the OS -- crypto today, with datatypes,
	// audio decode (MP3/Ogg/FLAC) and full app modules to follow -- and, in a
	// later phase, hosted ARM apps, all through the same scheduler. The pre-v2.x
	// REG_ZZ_ARM_RUN trampoline (upload a raw ARM blob and run it on core 1 in
	// an uncached environment) has been removed; the v2 ARM-hosted app platform
	// launches apps through the scheduler instead.
	C1_TRACE("[c1] R3 worker-start\r\n");
	scheduler_core1_worker();  // dual-core task worker; never returns
}
#pragma GCC pop_options

void arm_app_init() {
	arm_run_env.api_version = 1;
	arm_run_env.fn_putchar = _putchar;
	arm_run_env.fn_get_event_code = arm_app_get_event_code;
	arm_run_env.fn_get_event_serial = arm_app_get_event_serial;
	arm_run_env.fn_output_event_acked = arm_app_output_event_acked;
	arm_run_env.fn_put_event_code = arm_app_put_event_code;
	arm_run_env.fn_set_output_events_blocking =
			arm_app_set_output_events_blocking;
	arm_run_env.fn_set_output_putchar_to_events =
			arm_app_set_output_putchar_to_events;
	arm_run_env.argc = 0;

	Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_RESET,
			(Xil_ExceptionHandler) arm_exception_handler_id_reset, NULL);
	Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_DATA_ABORT_INT,
			(Xil_ExceptionHandler) arm_exception_handler_id_data_abort, NULL);
	Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_PREFETCH_ABORT_INT,
			(Xil_ExceptionHandler) arm_exception_handler_id_prefetch_abort, NULL);
	Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_UNDEFINED_INT,
			(Xil_ExceptionHandler) arm_exception_handler_illinst, NULL);

	printf("[core2] launch...\n");
	volatile uint32_t* core1_addr = (volatile uint32_t*) 0xFFFFFFF0;
	*core1_addr = (uint32_t) core1_entry;
	// Place some machine code in strategic positions that will catch core1 if it crashes
	// FIXME: clean this up and turn into a debug handler / monitor
	volatile uint32_t* core1_addr2 = (volatile uint32_t*) 0x140; // catch 1
	core1_addr2[0] = 0xe3e0000f; // mvn	r0, #15  -- loads 0xfffffff0
	core1_addr2[1] = 0xe590f000; // ldr	pc, [r0] -- jumps to the address in that address

	core1_addr2 = (volatile uint32_t*) 0x100; // catch 2
	core1_addr2[0] = 0xe3e0000f; // mvn	r0, #15  -- loads 0xfffffff0
	core1_addr2[1] = 0xe590f000; // ldr	pc, [r0] -- jumps to the address in that address

	// The reset-vector word and catch stubs live in memory CPU1 reads with its
	// caches off; flush core 0's D-cache so they are visible before we wake it.
	Xil_DCacheFlush();
	dsb();
	asm("sev");
	printf("[core2] now idling.\n");
}

void arm_app_input_event(uint32_t evt) {
	arm_app_input_event_code = evt;
	arm_app_input_event_serial++;
	arm_app_input_event_ack = 0;
}

uint32_t arm_app_output_event() {
	uint32_t data = (arm_app_output_event_serial << 16)
					| arm_app_output_event_code;

	arm_app_output_event_ack = 1;

	return data;
}

void arm_exception_handler_id_reset(void *callback) {
	record_fault(CORE_FAULT_RESET, "id_reset");
}

void arm_exception_handler_id_data_abort(void *callback) {
	record_fault(CORE_FAULT_DATA_ABORT, "id_data_abort");
}

void arm_exception_handler_id_prefetch_abort(void *callback) {
	record_fault(CORE_FAULT_PREFETCH_ABORT, "id_prefetch_abort");
}

void arm_exception_handler(void *callback) {
	record_fault(CORE_FAULT_GENERIC, "generic");
}

void arm_exception_handler_illinst(void *callback) {
	record_fault(CORE_FAULT_UNDEF, "illinst");
}
