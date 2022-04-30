#include <xscugic.h>

#ifndef ZZ_INTERRUPT_H
#define ZZ_INTERRUPT_H

// IRQ mask bits
#define AMIGA_INTERRUPT_ETH   1
#define AMIGA_INTERRUPT_AUDIO 2

XScuGic* interrupt_get_intc();
int interrupt_configure();
int fpga_interrupt_connect(void* isr_video, void* isr_audio_tx, void* isr_audio_rx);

void amiga_interrupt_set(uint32_t bit);
void amiga_interrupt_clear(uint32_t bit);
uint32_t amiga_interrupt_get();

#endif
