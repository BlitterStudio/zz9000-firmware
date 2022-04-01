
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
void arm_app_run(uint32_t arm_run_address);
void arm_app_input_event(uint32_t evt);
uint32_t arm_app_output_event();

void arm_exception_handler_id_reset(void *callback);
void arm_exception_handler_id_data_abort(void *callback);
void arm_exception_handler_id_prefetch_abort(void *callback);
void arm_exception_handler_illinst(void *callback);
