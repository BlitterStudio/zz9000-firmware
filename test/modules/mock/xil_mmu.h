#ifndef XIL_MMU_H
#define XIL_MMU_H
#include <stdint.h>
typedef uintptr_t UINTPTR;
#define NORM_NONCACHE 0U
static inline void Xil_SetTlbAttributes(UINTPTR address, uint32_t attributes)
{
    (void)address;
    (void)attributes;
}
#endif
