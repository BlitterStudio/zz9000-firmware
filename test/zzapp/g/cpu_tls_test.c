/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CPU_TLS_HOST_TEST
#include "cpu_tls.c"

/* The firmware's TLS image: libjpeg-turbo's two SIMD flags, both
 * initialised to ~0, read at tp + 8 + 0 and tp + 8 + 4. */
int main(void)
{
	static const uint32_t tdata[2] = { 0xffffffffu, 0xffffffffu };
	uint8_t block[CPU_TLS_TCB_BYTES + 24u];
	uint32_t word;
	uintptr_t tp;

	memset(block, 0xa5, sizeof(block));
	tp = cpu_tls_layout(block, sizeof(block), (const uint8_t *)tdata,
			    sizeof(tdata), sizeof(tdata), 12u);
	assert(tp == (uintptr_t)block);
	/* The thread control block is cleared. */
	for (word = 0; word < CPU_TLS_TCB_BYTES; word++)
		assert(block[word] == 0u);
	/* .tdata follows the TCB at the offsets the compiler uses. */
	memcpy(&word, (const uint8_t *)tp + 8u, 4u);
	assert(word == 0xffffffffu);
	memcpy(&word, (const uint8_t *)tp + 12u, 4u);
	assert(word == 0xffffffffu);
	/* .tbss is zeroed, and nothing past the image is touched. */
	for (word = 0; word < 12u; word++)
		assert(block[CPU_TLS_TCB_BYTES + 8u + word] == 0u);
	assert(block[CPU_TLS_TCB_BYTES + 20u] == 0xa5u);

	/* An image larger than the block is refused without writing. */
	memset(block, 0xa5, sizeof(block));
	assert(cpu_tls_layout(block, sizeof(block), (const uint8_t *)tdata,
			      sizeof(tdata), sizeof(tdata), 20u) == 0u);
	assert(block[0] == 0xa5u);
	/* A .tbss offset inside .tdata is refused. */
	assert(cpu_tls_layout(block, sizeof(block), (const uint8_t *)tdata,
			      sizeof(tdata), 4u, 0u) == 0u);

	printf("cpu_tls_test: ok\n");
	return 0;
}
