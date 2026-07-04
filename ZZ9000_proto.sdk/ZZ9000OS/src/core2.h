
struct ZZ9K_ENV {
	uint32_t api_version;
	uint32_t argv[8];
	uint32_t argc;

	int (*fn_putchar)(char);
	void (*fn_set_output_putchar_to_events)(char);
	void (*fn_set_output_events_blocking)(char);
	void (*fn_put_event_code)(uint16_t);
	uint16_t (*fn_get_event_serial)();
	uint16_t (*fn_get_event_code)();
	char (*fn_output_event_acked)();
};

void arm_app_init();
volatile struct ZZ9K_ENV* arm_app_get_run_env();
void core1_cold_restart(void);  /* re-reset core 1 into core1_loop (scheduler fault recovery) */
void arm_app_input_event(uint32_t evt);
uint32_t arm_app_output_event();

void arm_exception_handler_id_reset(void *callback);
void arm_exception_handler_id_data_abort(void *callback);
void arm_exception_handler_id_prefetch_abort(void *callback);
void arm_exception_handler_illinst(void *callback);

// Fault latch written by the exception handlers (usually crashed core1
// apps) and polled by the core0 main loop. The slot owns a whole cache
// line so core0 can invalidate-and-read it without touching its own data.
#define CORE_FAULT_NONE            0u
#define CORE_FAULT_RESET           1u
#define CORE_FAULT_DATA_ABORT      2u
#define CORE_FAULT_PREFETCH_ABORT  3u
#define CORE_FAULT_UNDEF           4u
#define CORE_FAULT_GENERIC         5u

struct core_fault_slot {
	volatile uint32_t code;
	uint32_t pad[7];
};
extern struct core_fault_slot core1_fault;
