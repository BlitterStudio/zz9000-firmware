#include <assert.h>
#include <stdint.h>

#include "overlay_color.h"

int main(void)
{
	assert(overlay_key_to_rgb(0x00123456U, MNTVA_COLOR_32BIT) ==
	       0x00123456U);

	/* Natural RGB565 values F800/07E0/001F are read by the little-endian ARM
	 * as 00F8/E007/1F00 from the big-endian P96 framebuffer. */
	assert(overlay_key_to_rgb(0x00f8U, MNTVA_COLOR_16BIT565) == 0x00ff0000U);
	assert(overlay_key_to_rgb(0xe007U, MNTVA_COLOR_16BIT565) == 0x0000ff00U);
	assert(overlay_key_to_rgb(0x1f00U, MNTVA_COLOR_16BIT565) == 0x000000ffU);

	/* RGB555 red/green/blue: 7C00/03E0/001F, likewise byte-swapped. */
	assert(overlay_key_to_rgb(0x007cU, MNTVA_COLOR_15BIT) == 0x00ff0000U);
	assert(overlay_key_to_rgb(0xe003U, MNTVA_COLOR_15BIT) == 0x0000ff00U);
	assert(overlay_key_to_rgb(0x1f00U, MNTVA_COLOR_15BIT) == 0x000000ffU);

	return 0;
}
