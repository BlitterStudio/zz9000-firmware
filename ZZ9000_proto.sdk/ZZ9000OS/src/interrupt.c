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

#define INTC_INTERRUPT_ID_0 61 // IRQ_F2P[0:0]
#define INTC_INTERRUPT_ID_1 62 // IRQ_F2P[1:1]
#define INTC_INTERRUPT_ID_2 63 // IRQ_F2P[2:2]

int fpga_interrupt_connect(void* isr_video, void* isr_audio_tx, void* isr_audio_rx) {
  int result;
  XScuGic *intc_instance_ptr = interrupt_get_intc();

  printf("XScuGic_SetPriorityTriggerType()\n");

  // set the priority of IRQ_F2P[0:0] to 0xA0 (highest 0xF8, lowest 0x00) and a trigger for a rising edge 0x3.
  XScuGic_SetPriorityTriggerType(intc_instance_ptr, INTC_INTERRUPT_ID_0, 0xA0, 0x3); // vblank / split
  XScuGic_SetPriorityTriggerType(intc_instance_ptr, INTC_INTERRUPT_ID_1, 0x90, 0x3); // audio formatter TX
  XScuGic_SetPriorityTriggerType(intc_instance_ptr, INTC_INTERRUPT_ID_2, 0x90, 0x3); // audio formatter RX

  printf("XScuGic_Connect()\n");

  // connect the interrupt service routine isr0 to the interrupt controller
  result = XScuGic_Connect(intc_instance_ptr, INTC_INTERRUPT_ID_0, (Xil_ExceptionHandler)isr_video, NULL);
  result = XScuGic_Connect(intc_instance_ptr, INTC_INTERRUPT_ID_1, (Xil_ExceptionHandler)isr_audio_tx, NULL);
  result = XScuGic_Connect(intc_instance_ptr, INTC_INTERRUPT_ID_2, (Xil_ExceptionHandler)isr_audio_rx, NULL);

  if (result != XST_SUCCESS) {
	printf("XScuGic_Connect() failed!\n");
    return result;
  }

  printf("XScuGic_Enable()\n");

  // enable interrupts for IRQ_F2P[0:0]
  XScuGic_Enable(intc_instance_ptr, INTC_INTERRUPT_ID_0);
  // enable interrupts for IRQ_F2P[1:1]
  XScuGic_Enable(intc_instance_ptr, INTC_INTERRUPT_ID_1);
  // enable interrupts for IRQ_F2P[2:2]
  XScuGic_Enable(intc_instance_ptr, INTC_INTERRUPT_ID_2);

  return 0;
}
