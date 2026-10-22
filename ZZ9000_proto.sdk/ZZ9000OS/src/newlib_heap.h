/* Newlib heap high-water accounting. */
#ifndef NEWLIB_HEAP_H
#define NEWLIB_HEAP_H

#include <stdint.h>

/* Returns current break consumption and the greatest successful break since
 * boot. The snapshot is serialized with newlib's malloc lock. */
void newlib_heap_usage(uint32_t *used, uint32_t *peak);

#endif /* NEWLIB_HEAP_H */
