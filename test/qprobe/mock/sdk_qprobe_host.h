/* Host hook for sdk_qprobe.h: the state window is a static array and ARM
 * addresses of shared buffers map to fake memory. */
#ifndef SDK_QPROBE_HOST_H
#define SDK_QPROBE_HOST_H
#include <stdint.h>
extern uint8_t qprobe_host_state[0x2000] __attribute__((aligned(64)));
volatile void *qprobe_host_ptr(uint32_t address);
#define SDK_QPROBE_STATE_BASE ((void *)qprobe_host_state)
#define SDK_QPROBE_PTR(address) qprobe_host_ptr(address)
#endif
