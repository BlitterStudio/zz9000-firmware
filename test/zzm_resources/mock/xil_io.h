#ifndef XIL_IO_H
#define XIL_IO_H
#include <stdint.h>
uint32_t Xil_In32(uintptr_t address);
void Xil_Out32(uintptr_t address, uint32_t value);
#endif
