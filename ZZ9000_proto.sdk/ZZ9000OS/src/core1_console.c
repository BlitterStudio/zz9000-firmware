#include "core1_console.h"

#include <stdint.h>
#include <string.h>

#ifndef CORE1_CONSOLE_HOST_TEST
#include "xparameters.h"
#include "xuartps_hw.h"
#endif

#define CORE1_CONSOLE_RING_BYTES 16384u
#define CORE1_CONSOLE_LINE_BYTES 1024u
#ifndef CORE1_CONSOLE_DRAIN_BYTES
#define CORE1_CONSOLE_DRAIN_BYTES 64u
#endif

struct core1_console_state {
	uint8_t ring[CORE1_CONSOLE_RING_BYTES];
	uint32_t head;
	uint32_t tail;
	uint8_t line[CORE1_CONSOLE_LINE_BYTES];
	uint32_t line_len;
	uint32_t line_discard;
	uint32_t producer_busy;
	uint32_t dropped_lines;
	char drop_report[48];
	uint32_t drop_report_len;
	uint32_t drop_report_pos;
	uint32_t consumer_line_active;
};

static struct core1_console_state console __attribute__((aligned(64)));

#ifdef CORE1_CONSOLE_HOST_TEST
extern uint32_t core1_console_host_cpu(void);
extern int core1_console_host_uart_is_full(void);
extern void core1_console_host_uart_send(uint8_t c);
extern void core1_console_host_publish_preempt(void);
extern uint32_t core1_console_host_irq_save(void);
extern void core1_console_host_irq_restore(uint32_t state);
extern void core1_console_host_consumer_irq(uint8_t c);
extern void core1_console_host_flush_produce(void);

static uint32_t console_cpu(void)
{
	return core1_console_host_cpu();
}

static uint32_t irq_save(void)
{
	return core1_console_host_irq_save();
}

static void irq_restore(uint32_t state)
{
	core1_console_host_irq_restore(state);
}

static int uart_is_full(void)
{
	return core1_console_host_uart_is_full();
}

static void uart_write_fifo(uint8_t c)
{
	core1_console_host_uart_send(c);
	core1_console_host_consumer_irq(c);
}

static void uart_send_sync(uint8_t c)
{
	core1_console_host_uart_send(c);
}
#else
static uint32_t console_cpu(void)
{
	uint32_t mpidr;

	__asm__ __volatile__("mrc p15, 0, %0, c0, c0, 5" : "=r"(mpidr));
	return mpidr & 3u;
}

static uint32_t irq_save(void)
{
	uint32_t cpsr;

	__asm__ __volatile__("mrs %0, cpsr\n\tcpsid i" : "=r"(cpsr) : : "memory");
	return cpsr;
}

static void irq_restore(uint32_t state)
{
	__asm__ __volatile__("msr cpsr_c, %0" : : "r"(state) : "memory");
}

static int uart_is_full(void)
{
	return XUartPs_IsTransmitFull(STDOUT_BASEADDRESS);
}

static void uart_write_fifo(uint8_t c)
{
	XUartPs_WriteReg(STDOUT_BASEADDRESS, XUARTPS_FIFO_OFFSET, c);
}

static void uart_send_sync(uint8_t c)
{
	XUartPs_SendByte(STDOUT_BASEADDRESS, c);
}
#endif

void core1_console_init(void)
{
	memset(&console, 0, sizeof(console));
}

void core1_console_reset_producer(void)
{
	console.line_len = 0u;
	console.line_discard = 0u;
	console.producer_busy = 0u;
}

static void count_dropped_line(void)
{
	(void)__atomic_fetch_add(&console.dropped_lines, 1u, __ATOMIC_RELAXED);
}

static void publish_line(void)
{
	uint32_t head = __atomic_load_n(&console.head, __ATOMIC_RELAXED);
	uint32_t tail = __atomic_load_n(&console.tail, __ATOMIC_ACQUIRE);
	uint32_t i;

#ifdef CORE1_CONSOLE_HOST_TEST
	core1_console_host_publish_preempt();
#endif
	if (console.line_len > CORE1_CONSOLE_RING_BYTES - (head - tail)) {
		count_dropped_line();
		return;
	}
	for (i = 0u; i < console.line_len; i++)
		console.ring[(head + i) & (CORE1_CONSOLE_RING_BYTES - 1u)] =
			console.line[i];
	__atomic_store_n(&console.head, head + console.line_len, __ATOMIC_RELEASE);
}

static void append_core1_byte(uint8_t c)
{
	if (console.line_discard != 0u) {
		if (c == '\n') {
			console.line_discard = 0u;
			count_dropped_line();
		}
		return;
	}
	if (console.line_len == CORE1_CONSOLE_LINE_BYTES) {
		console.line_len = 0u;
		console.line_discard = 1u;
		if (c == '\n') {
			console.line_discard = 0u;
			count_dropped_line();
		}
		return;
	}
	console.line[console.line_len++] = c;
	if (c == '\n') {
		publish_line();
		console.line_len = 0u;
	}
}

static void append_reentrant_byte(uint8_t c)
{
	if (c == '\n')
		count_dropped_line();
}

static void append_core1_output(uint8_t c)
{
	uint32_t irq_state = irq_save();

	if (console.producer_busy != 0u)
		append_reentrant_byte(c);
	else {
		console.producer_busy = 1u;
		append_core1_byte(c);
		console.producer_busy = 0u;
	}
	irq_restore(irq_state);
}

static void format_drop_report(uint32_t count)
{
	static const char prefix[] = "[console] dropped ";
	static const char suffix[] = " core-1 lines\n";
	char digits[10];
	uint32_t n = 0u;
	uint32_t i;

	if (count == 0u)
		return;
	memcpy(console.drop_report, prefix, sizeof(prefix) - 1u);
	console.drop_report_len = sizeof(prefix) - 1u;
	do {
		digits[n++] = (char)('0' + count % 10u);
		count /= 10u;
	} while (count != 0u);
	for (i = n; i != 0u; i--)
		console.drop_report[console.drop_report_len++] = digits[i - 1u];
	memcpy(console.drop_report + console.drop_report_len, suffix,
	       sizeof(suffix) - 1u);
	console.drop_report_len += sizeof(suffix) - 1u;
	console.drop_report_pos = 0u;
}

static void begin_drop_report(void)
{
	uint32_t count;

	if (console.drop_report_pos != console.drop_report_len)
		return;
	count = __atomic_exchange_n(&console.dropped_lines, 0u, __ATOMIC_ACQ_REL);
	format_drop_report(count);
}

static void consumer_sent_byte(uint8_t c)
{
	uart_write_fifo(c);
	console.consumer_line_active = c == '\n' ? 0u : 1u;
}

static int drain_one(int synchronous)
{
	uint32_t irq_state;
	uint32_t tail;
	uint32_t head;
	uint8_t c;

	for (;;) {
		if (synchronous) {
			while (uart_is_full()) {
			}
		}
		irq_state = irq_save();
		if (!uart_is_full())
			break;
		irq_restore(irq_state);
		if (!synchronous)
			return 0;
	}
	if (console.drop_report_pos != console.drop_report_len) {
		c = (uint8_t)console.drop_report[console.drop_report_pos++];
		consumer_sent_byte(c);
		irq_restore(irq_state);
		return 1;
	}
	tail = __atomic_load_n(&console.tail, __ATOMIC_RELAXED);
	head = __atomic_load_n(&console.head, __ATOMIC_ACQUIRE);
	if (tail == head) {
		begin_drop_report();
		if (console.drop_report_pos == console.drop_report_len) {
			irq_restore(irq_state);
			return 0;
		}
		c = (uint8_t)console.drop_report[console.drop_report_pos++];
		consumer_sent_byte(c);
		irq_restore(irq_state);
		return 1;
	}
	c = console.ring[tail & (CORE1_CONSOLE_RING_BYTES - 1u)];
	consumer_sent_byte(c);
	__atomic_store_n(&console.tail, tail + 1u, __ATOMIC_RELEASE);
	irq_restore(irq_state);
	return 1;
}

static int drain_snapshot_one(uint32_t snapshot_head)
{
	uint32_t irq_state;
	uint32_t tail;
	uint8_t c;

	for (;;) {
		while (uart_is_full()) {
		}
		irq_state = irq_save();
		if (!uart_is_full())
			break;
		irq_restore(irq_state);
	}
	if (console.drop_report_pos != console.drop_report_len) {
		c = (uint8_t)console.drop_report[console.drop_report_pos++];
		consumer_sent_byte(c);
		irq_restore(irq_state);
		return 1;
	}
	tail = __atomic_load_n(&console.tail, __ATOMIC_RELAXED);
	if (tail == snapshot_head) {
		irq_restore(irq_state);
		return 0;
	}
	c = console.ring[tail & (CORE1_CONSOLE_RING_BYTES - 1u)];
	consumer_sent_byte(c);
	__atomic_store_n(&console.tail, tail + 1u, __ATOMIC_RELEASE);
	irq_restore(irq_state);
	return 1;
}

void core1_console_drain(void)
{
	uint32_t budget;

	for (budget = 0u; budget < CORE1_CONSOLE_DRAIN_BYTES; budget++) {
		if (!drain_one(0))
			return;
	}
}

void core1_console_flush_sync(void)
{
	uint32_t snapshot_head =
		__atomic_load_n(&console.head, __ATOMIC_ACQUIRE);
	uint32_t snapshot_drops =
		__atomic_exchange_n(&console.dropped_lines, 0u, __ATOMIC_ACQ_REL);

#ifdef CORE1_CONSOLE_HOST_TEST
	core1_console_host_flush_produce();
#endif
	while (drain_snapshot_one(snapshot_head)) {
	}
	format_drop_report(snapshot_drops);
	while (drain_snapshot_one(snapshot_head)) {
	}
}

/* The BSP's weak write() and _write(), and xil_printf(), all call outbyte().
 * Core 0 remains synchronous, including panic and data-abort diagnostics. */
void outbyte(char c)
{
	if (console_cpu() == 1u) {
		append_core1_output((uint8_t)c);
		return;
	}
	if (console.consumer_line_active != 0u) {
		while (console.consumer_line_active != 0u)
			(void)drain_one(1);
	}
	uart_send_sync((uint8_t)c);
}
