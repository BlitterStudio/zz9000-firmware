#include <xscugic.h>

#ifndef ZZ_INTERRUPT_H
#define ZZ_INTERRUPT_H

XScuGic* interrupt_get_intc();
int interrupt_configure();
int fpga_interrupt_connect(void* isr_video, void* isr_audio_tx, void* isr_audio_rx);

#endif
