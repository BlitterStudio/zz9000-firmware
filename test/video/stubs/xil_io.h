/* Host-test replacement for the Xilinx MMIO helpers. */
#ifndef TEST_XIL_IO_H
#define TEST_XIL_IO_H

#include <stdint.h>

void test_xil_out32(uintptr_t address, uint32_t value);
uint32_t test_xil_in32(uintptr_t address);

#define Xil_Out32(address, value) \
	test_xil_out32((uintptr_t)(address), (uint32_t)(value))
#define Xil_In32(address) test_xil_in32((uintptr_t)(address))

#endif
