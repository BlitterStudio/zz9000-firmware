#include <stdio.h>
#include "interrupt.h"

static XScuGic intc_handle;

XScuGic* interrupt_get_intc() {
	return &intc_handle;
}

int interrupt_configure() {
	int result;
	XScuGic *intc = interrupt_get_intc();
	XScuGic_Config *intc_config;

	intc_config = XScuGic_LookupConfig(XPAR_PS7_SCUGIC_0_DEVICE_ID);
	if (!intc_config) {
		return XST_FAILURE;
	}

	printf("XScuGic_CfgInitialize()\n");
	result = XScuGic_CfgInitialize(intc, intc_config, intc_config->CpuBaseAddress);

	if (result != XST_SUCCESS) {
		return result;
	}

	Xil_ExceptionInit();

	/*
	* Connect the interrupt controller interrupt handler to the hardware
	* interrupt handling logic in the processor.
	*/
	Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_IRQ_INT,
		(Xil_ExceptionHandler)XScuGic_InterruptHandler,
		intc);

  return 0;
}
