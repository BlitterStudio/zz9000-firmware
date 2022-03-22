#include <stdint.h>

int audio_adau_init(uint32_t* audio_buffer);
void isr_audio(void *dummy);
void isr_audio_rx(void *dummy);
void audio_set_interrupt_enabled(int en);
void audio_clear_interrupt();
uint32_t audio_get_interrupt();
uint32_t audio_get_dma_transfer_count();
int audio_swab(int audio_scale, uint32_t offset);
