#ifndef _LINUX_COMPILER_H
#define _LINUX_COMPILER_H
#define __packed __attribute__((packed))
#define __aligned(x) __attribute__((aligned(x)))
#define __must_check __attribute__((warn_unused_result))
#define __bitwise
#define __force
#define __iomem
#ifndef noinline
#define noinline __attribute__((noinline))
#endif
#endif
