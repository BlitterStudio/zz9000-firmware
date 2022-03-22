#include <stdint.h>
#include <stdio.h>
#include "xil_cache.h"
#include "xil_exception.h"
#include "xil_io.h"
#include "xil_misc_psreset_api.h"
#include "core2.h"
#include "sleep.h"

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

#pragma GCC push_options
#pragma GCC optimize ("O1")
// core1_loop is executed on core1 (vs core0)
void core1_loop() {
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

	// stack
	asm("mov sp, #0x06000000");

	volatile uint32_t* addr = 0;
	addr[0] = 0xe3e0000f; // mvn	r0, #15  -- loads 0xfffffff0
	addr[1] = 0xe590f000; // ldr	pc, [r0] -- jumps to the address in that address

	while (1) {
		while (!core2_execute) {
			usleep(1);
		}
		core2_execute = 0;
		printf("[core2] executing at %p.\n", core1_trampoline);
		Xil_DCacheFlush();
		Xil_ICacheInvalidate();

		asm("push {r0-r12}");
		// FIXME HACK save our stack pointer in 0x10000
		asm("mov r0, #0x00010000");
		asm("str sp, [r0]");

		core1_trampoline(&arm_run_env);

		asm("mov r0, #0x00010000");
		asm("ldr sp, [r0]");
		asm("pop {r0-r12}");
	}
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
	*core1_addr = (uint32_t) core1_loop;
	// Place some machine code in strategic positions that will catch core1 if it crashes
	// FIXME: clean this up and turn into a debug handler / monitor
	volatile uint32_t* core1_addr2 = (volatile uint32_t*) 0x140; // catch 1
	core1_addr2[0] = 0xe3e0000f; // mvn	r0, #15  -- loads 0xfffffff0
	core1_addr2[1] = 0xe590f000; // ldr	pc, [r0] -- jumps to the address in that address

	core1_addr2 = (volatile uint32_t*) 0x100; // catch 2
	core1_addr2[0] = 0xe3e0000f; // mvn	r0, #15  -- loads 0xfffffff0
	core1_addr2[1] = 0xe590f000; // ldr	pc, [r0] -- jumps to the address in that address

	asm("sev");
	printf("[core2] now idling.\n");
}

void arm_app_run(uint32_t arm_run_address) {
	volatile uint32_t* core1_addr = (volatile uint32_t*) 0xFFFFFFF0;
	volatile uint32_t* core1_addr2 = (volatile uint32_t*) 0x100; // catch 2

	*core1_addr = (uint32_t) core1_loop;
	core1_addr2[0] = 0xe3e0000f; // mvn	r0, #15  -- loads 0xfffffff0
	core1_addr2[1] = 0xe590f000; // ldr	pc, [r0] -- jumps to the address in that address

	printf("[ARM_RUN] %lx\n", arm_run_address);
	if (arm_run_address > 0) {
		core1_trampoline = (volatile void (*)(
				volatile struct ZZ9K_ENV*)) arm_run_address;
		printf("[ARM_RUN] signaling second core.\n");
		Xil_DCacheFlush();
		Xil_ICacheInvalidate();
		core2_execute = 1;
		Xil_DCacheFlush();
		Xil_ICacheInvalidate();
	} else {
		core1_trampoline = 0;
		core2_execute = 0;
	}

	// FIXME move this out of here
	// sequence to reset cpu1 taken from https://xilinx-wiki.atlassian.net/wiki/spaces/A/pages/18842504/XAPP1079+Latest+Information

	Xil_Out32(XSLCR_UNLOCK_ADDR, XSLCR_UNLOCK_CODE);
	uint32_t RegVal = Xil_In32(A9_CPU_RST_CTRL);
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
	printf("id_reset: arm_exception_handler()!\n");
	while (1) {
	}
}

void arm_exception_handler_id_data_abort(void *callback) {
	printf("id_data_abort: arm_exception_handler()!\n");
	while (1) {
	}
}

void arm_exception_handler_id_prefetch_abort(void *callback) {
	printf("id_prefetch_abort: arm_exception_handler()!\n");
	while (1) {
	}
}

void arm_exception_handler(void *callback) {
	printf("arm_exception_handler()!\n");
	while (1) {
	}
}

void arm_exception_handler_illinst(void *callback) {
	printf("arm_exception_handler_illinst()!\n");
	while (1) {
	}
}
