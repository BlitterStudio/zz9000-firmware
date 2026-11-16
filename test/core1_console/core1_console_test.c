#include "core1_console.h"

#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define OUTPUT_CAP 65536u

static __thread uint32_t test_cpu;
static int uart_full;
static unsigned uart_full_checks;
static unsigned uart_sends;
static char output[OUTPUT_CAP];
static size_t output_len;
static int inject_preemption;
static uint32_t irq_masked;
static int inject_consumer_irq;
static int pending_consumer_irq;
static int inject_flush_producer;
static int inject_full_between_wait_and_mask;
static int clear_full_on_check;
static unsigned writes_while_full;
static void emit(const char *text);

uint32_t core1_console_host_cpu(void)
{
	return test_cpu;
}

int core1_console_host_uart_is_full(void)
{
	int full;

	uart_full_checks++;
	full = uart_full;
	if (full != 0 && clear_full_on_check != 0) {
		clear_full_on_check = 0;
		uart_full = 0;
	}
	return full;
}

void core1_console_host_uart_send(uint8_t c)
{
	if (uart_full != 0)
		writes_while_full++;
	if (output_len < sizeof(output))
		output[output_len++] = (char)c;
	uart_sends++;
}
uint32_t core1_console_host_irq_save(void)
{
	uint32_t previous = irq_masked;

	irq_masked = 1u;
	if (inject_full_between_wait_and_mask != 0) {
		inject_full_between_wait_and_mask = 0;
		uart_full = 1;
		clear_full_on_check = 1;
	}
	return previous;
}

void core1_console_host_irq_restore(uint32_t state)
{
	irq_masked = state;
	if (irq_masked == 0u && pending_consumer_irq != 0) {
		pending_consumer_irq = 0;
		test_cpu = 0u;
		emit("IRQ\n");
	}
}

void core1_console_host_consumer_irq(uint8_t c)
{
	(void)c;
	if (inject_consumer_irq != 0) {
		inject_consumer_irq = 0;
		if (irq_masked != 0u)
			pending_consumer_irq = 1;
		else
			emit("IRQ\n");
	}
}

void core1_console_host_flush_produce(void)
{
	if (inject_flush_producer != 0) {
		inject_flush_producer = 0;
		test_cpu = 1u;
		emit("late\n");
		test_cpu = 0u;
	}
}



/* Called by the production publication seam after it sampled head/tail. */
void core1_console_host_publish_preempt(void)
{
	if (inject_preemption != 0) {
		inject_preemption = 0;
		emit("nested\n");
	}
}

static int failures;

#define CHECK(condition) do { \
	if (!(condition)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
		failures++; \
	} \
} while (0)

static void reset_console(void)
{
	core1_console_init();
	test_cpu = 1u;
	uart_full = 0;
	uart_full_checks = 0u;
	uart_sends = 0u;
	output_len = 0u;
	inject_preemption = 0;
	irq_masked = 0u;
	inject_consumer_irq = 0;
	pending_consumer_irq = 0;
	inject_flush_producer = 0;
	inject_full_between_wait_and_mask = 0;
	clear_full_on_check = 0;
	writes_while_full = 0u;
}

static void emit(const char *text)
{
	while (*text != '\0')
		outbyte(*text++);
}

static void drain_once(void)
{
	test_cpu = 0u;
	core1_console_drain();
	test_cpu = 1u;
}

static void drain_times(unsigned count)
{
	while (count-- != 0u)
		drain_once();
}

static void test_order_and_wrap(void)
{
	unsigned i;
	char line[32];
	char expected[20000];
	size_t expected_len = 0u;

	reset_console();
	for (i = 0u; i < 1800u; i++) {
		int n = snprintf(line, sizeof(line), "line-%04u\n", i);

		CHECK(n > 0 && (size_t)n < sizeof(line));
		memcpy(expected + expected_len, line, (size_t)n);
		expected_len += (size_t)n;
		emit(line);
		drain_times(3u);
	}
	CHECK(output_len == expected_len);
	CHECK(memcmp(output, expected, expected_len) == 0);
}

static void test_whole_line_drop_and_report(void)
{
	char full_line[129];
	const char report[] = "[console] dropped 1 core-1 lines\n";
	const char appended[] = "new\n";
	unsigned i;

	reset_console();
	memset(full_line, 'x', sizeof(full_line) - 1u);
	full_line[sizeof(full_line) - 2u] = '\n';
	full_line[sizeof(full_line) - 1u] = '\0';
	for (i = 0u; i < 128u; i++)
		emit(full_line);
	emit("discarded\n");
	/* 16 KiB then four report bytes: leave the report in flight. */
	drain_times(4097u);
	CHECK(output_len == 128u * (sizeof(full_line) - 1u) + 4u);
	emit(appended);
	drain_times(32u);
	CHECK(memcmp(output, full_line, sizeof(full_line) - 1u) == 0);
	CHECK(memcmp(output + 128u * (sizeof(full_line) - 1u), report,
	             sizeof(report) - 1u) == 0);
	CHECK(memcmp(output + 128u * (sizeof(full_line) - 1u) +
	             sizeof(report) - 1u, appended, sizeof(appended) - 1u) == 0);
}

static void test_reentrant_publish_drops_nested_line(void)
{
	const char expected[] = "outer\n[console] dropped 1 core-1 lines\n";

	reset_console();
	inject_preemption = 1;
	emit("outer\n");
	drain_times(16u);
	CHECK(output_len == sizeof(expected) - 1u);
	CHECK(memcmp(output, expected, sizeof(expected) - 1u) == 0);
}

static void test_restart_keeps_queued_lines(void)
{
	const char expected[] = "before restart\nafter restart\n";

	reset_console();
	emit("before restart\n");
	core1_console_reset_producer();
	emit("after restart\n");
	drain_times(16u);
	CHECK(output_len == sizeof(expected) - 1u);
	CHECK(memcmp(output, expected, sizeof(expected) - 1u) == 0);
}

static void test_core1_never_waits_for_busy_uart(void)
{
	const char expected[] = "busy uart must not stall core one\n";

	reset_console();
	uart_full = 1;
	emit(expected);
	CHECK(uart_full_checks == 0u);
	CHECK(uart_sends == 0u);
	uart_full = 0;
	drain_times(16u);
	CHECK(output_len == sizeof(expected) - 1u);
	CHECK(memcmp(output, expected, sizeof(expected) - 1u) == 0);
}

static void test_drain_checks_busy_uart_and_budgets_pass(void)
{
	const char expected[] = "consumer busy\n";

	reset_console();
	emit(expected);
	test_cpu = 0u;
	uart_full = 1;
	core1_console_drain();
	test_cpu = 1u;
	CHECK(uart_full_checks != 0u);
	CHECK(uart_sends == 0u);
	uart_full = 0;
	drain_once();
	CHECK(uart_sends == CORE1_CONSOLE_DRAIN_BYTES);
	drain_times(16u);
	CHECK(output_len == sizeof(expected) - 1u);
	CHECK(memcmp(output, expected, sizeof(expected) - 1u) == 0);
}

static void test_core0_finishes_active_core1_line(void)
{
	const char expected[] = "ABCDEFGHIJ\ncore0\n";

	reset_console();
	emit("ABCDEFGHIJ\n");
	drain_once();
	CHECK(output_len == CORE1_CONSOLE_DRAIN_BYTES);
	test_cpu = 0u;
	emit("core0\n");
	test_cpu = 1u;
	CHECK(output_len == sizeof(expected) - 1u);
	CHECK(memcmp(output, expected, sizeof(expected) - 1u) == 0);
}
static void test_sync_drain_rechecks_after_irq_mask(void)
{
	const char expected[] = "ABCDEFGHIJ\nx\n";

	reset_console();
	emit("ABCDEFGHIJ\n");
	drain_once();
	inject_full_between_wait_and_mask = 1;
	test_cpu = 0u;
	emit("x\n");
	test_cpu = 1u;
	CHECK(writes_while_full == 0u);
	CHECK(output_len == sizeof(expected) - 1u);
	CHECK(memcmp(output, expected, sizeof(expected) - 1u) == 0);
}

static void test_core0_irq_reentry_keeps_consumer_state(void)
{
	const char expected[] = "abc\nIRQ\n";

	reset_console();
	emit("abc\n");
	inject_consumer_irq = 1;
	drain_once();
	CHECK(output_len == sizeof(expected) - 1u);
	CHECK(memcmp(output, expected, sizeof(expected) - 1u) == 0);
}


static void test_core0_fault_flushes_snapshot(void)
{
	const char expected[] = "queued before fault\nlate\n";

	reset_console();
	emit("queued before fault\n");
	inject_flush_producer = 1;
	test_cpu = 0u;
	core1_console_flush_sync();
	test_cpu = 1u;
	CHECK(output_len == sizeof("queued before fault\n") - 1u);
	CHECK(memcmp(output, "queued before fault\n",
	             sizeof("queued before fault\n") - 1u) == 0);
	drain_times(16u);
	CHECK(output_len == sizeof(expected) - 1u);
	CHECK(memcmp(output, expected, sizeof(expected) - 1u) == 0);
}
static void test_snapshot_drain_rechecks_after_irq_mask(void)
{
	const char expected[] = "snapshot\n";

	reset_console();
	emit(expected);
	inject_full_between_wait_and_mask = 1;
	test_cpu = 0u;
	core1_console_flush_sync();
	test_cpu = 1u;
	CHECK(writes_while_full == 0u);
	CHECK(output_len == sizeof(expected) - 1u);
	CHECK(memcmp(output, expected, sizeof(expected) - 1u) == 0);
}


struct concurrent_state {
	int done;
	char expected[12000];
	size_t expected_len;
};

static void *producer(void *opaque)
{
	struct concurrent_state *state = opaque;
	unsigned i;
	char line[32];

	test_cpu = 1u;
	for (i = 0u; i < 1000u; i++) {
		int n = snprintf(line, sizeof(line), "work-%04u\n", i);

		emit(line);
		memcpy(state->expected + state->expected_len, line, (size_t)n);
		state->expected_len += (size_t)n;
		if ((i & 15u) == 15u)
			sched_yield();
	}
	__atomic_store_n(&state->done, 1, __ATOMIC_RELEASE);
	return NULL;
}

static void *consumer(void *opaque)
{
	struct concurrent_state *state = opaque;

	test_cpu = 0u;
	for (;;) {
		size_t expected_len;

		core1_console_drain();
		if (__atomic_load_n(&state->done, __ATOMIC_ACQUIRE) != 0) {
			expected_len = state->expected_len;
			if (output_len == expected_len)
				break;
		}
		sched_yield();
	}
	return NULL;
}

static void test_concurrent_producer_consumer(void)
{
	struct concurrent_state state;
	pthread_t producer_thread;
	pthread_t consumer_thread;

	reset_console();
	memset(&state, 0, sizeof(state));
	CHECK(pthread_create(&producer_thread, NULL, producer, &state) == 0);
	CHECK(pthread_create(&consumer_thread, NULL, consumer, &state) == 0);
	CHECK(pthread_join(producer_thread, NULL) == 0);
	CHECK(pthread_join(consumer_thread, NULL) == 0);
	CHECK(output_len == state.expected_len);
	CHECK(memcmp(output, state.expected, state.expected_len) == 0);
}

int main(void)
{
	test_order_and_wrap();
	test_whole_line_drop_and_report();
	test_reentrant_publish_drops_nested_line();
	test_restart_keeps_queued_lines();
	test_core1_never_waits_for_busy_uart();
	test_drain_checks_busy_uart_and_budgets_pass();
	test_core0_finishes_active_core1_line();
	test_sync_drain_rechecks_after_irq_mask();
	test_core0_irq_reentry_keeps_consumer_state();
	test_core0_fault_flushes_snapshot();
	test_snapshot_drain_rechecks_after_irq_mask();
	test_concurrent_producer_consumer();
	if (failures != 0)
		return 1;
	puts("core1 console tests passed");
	return 0;
}
