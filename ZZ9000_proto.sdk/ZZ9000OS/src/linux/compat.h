#ifndef _LINUX_COMPAT_H
#define _LINUX_COMPAT_H
#include <stdint.h>
#include <stdlib.h>
typedef uint32_t dma_addr_t;
typedef unsigned long ulong;
#define dma_map_single(dev,ptr,size,dir) ((dma_addr_t)(uintptr_t)(ptr))
#define dma_unmap_single(dev,addr,size,dir)
#endif
