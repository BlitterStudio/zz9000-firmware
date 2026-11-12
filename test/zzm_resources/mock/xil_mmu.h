#ifndef XIL_MMU_H
#define XIL_MMU_H
#include <stdint.h>
#define DEVICE_MEMORY 0U
void Xil_SetTlbAttributes(uintptr_t address, uint32_t attributes);
#endif
