#include "zz_video_modes.h"

/* Keep the transmitter dark while a new formatter/pixel-clock mode is being
 * programmed, then expose the signal only after the clock has locked. */
void hdmi_ctrl_prepare_mode(struct zz_video_mode *mode);
void hdmi_ctrl_enable_output(void);
