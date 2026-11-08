#ifndef _LINUX_COMPILER_H
#define _LINUX_COMPILER_H
/* newlib's <sys/cdefs.h> defines both with the same meaning. */
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#ifndef __aligned
#define __aligned(x) __attribute__((aligned(x)))
#endif
#define __must_check __attribute__((warn_unused_result))
#define __bitwise
#define __force
#define __iomem
#ifndef noinline
#define noinline __attribute__((noinline))
#endif
#endif
