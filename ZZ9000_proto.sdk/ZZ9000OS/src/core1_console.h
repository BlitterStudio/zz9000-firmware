/*
 * Core-1 UART output is queued here and drained by core 0.  The strong
 * outbyte() replacement in core1_console.c catches newlib and xil_printf.
 */
#ifndef CORE1_CONSOLE_H
#define CORE1_CONSOLE_H

void core1_console_init(void);
void core1_console_drain(void);
void core1_console_flush_sync(void);
void core1_console_reset_producer(void);
void outbyte(char c);

#endif
