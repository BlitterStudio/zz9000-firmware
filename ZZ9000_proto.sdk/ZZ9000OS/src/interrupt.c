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
  return 0;
}
