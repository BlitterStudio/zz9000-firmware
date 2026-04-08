#ifndef _LINUX_KERNEL_H
#define _LINUX_KERNEL_H
#include <stdint.h>
#include <stddef.h>
#ifndef min
#define min(a,b) ((a)<(b)?(a):(b))
#endif
#ifndef max
#define max(a,b) ((a)>(b)?(a):(b))
#endif
#define ARRAY_SIZE(arr) (sizeof(arr)/sizeof((arr)[0]))
#define DIV_ROUND_UP(n,d) (((n)+(d)-1)/(d))
#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))
#endif
