/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Claim admission and the window layer over process records (slice 5b).
 *
 * A claim reserves, in order, a window (LIMIT), a record slot (MEMORY), the
 * L2 reserve (TABLES) and the window memory (HOSTHEAP), and publishes nothing
 * until all four are held. Each refusal reason is driven here with the
 * resource it names exhausted, and must leave every counter, the handle table
 * and every lifecycle block as it was. Load-time refusals (staging, tables)
 * end the claim and give everything back. With the window ceiling lifted
 * (-DZZAPP_SLOT_COUNT=64u) many apps are live at once and every stop, exit,
 * reset and quarantine must return to the counts QUERY reports.
 *
 * The services run on the stub kernel and loader of ZZAPP_SERVICES_STANDALONE_TEST,
 * over the real record arena and table layers. The host's retire path is the
 * double in svc_env.h (tables back, wire layer detached and freed).
 */
#define ZZAPP_SERVICES_STANDALONE_TEST
#include "zzapp_services.c"

#include "svc_env.h"

#define CHECK(expr) do { \
	checks++; \
	if (!(expr)) { \
		failures++; \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
	} \
} while (0)

static int checks;
static int failures;

static uint8_t win_mem[ZZAPP_SLOT_COUNT][ZZAPP_WINDOW_BYTES];

/* ---- Host-heap model ----------------------------------------------------- */

/* The surface allocator hands each window its fixed range. A test occupies a
 * range through zzapp_test_card_alloc_at() to make a claim's last step fail. */
static uint8_t range_taken[ZZAPP_SLOT_COUNT];
static uint32_t range_serial[ZZAPP_SLOT_COUNT];
static uint32_t next_serial;

static int range_of(uint32_t addr)
{
	uint32_t win;

	for (win = 0U; win < ZZAPP_SLOT_COUNT; win++) {
		if (ZZAPP_WINDOW_Z3_SLOT_ADDRESS(win) == addr)
			return (int)win;
	}
	return -1;
}

uint32_t zzapp_test_card_alloc_at(uint32_t addr, uint32_t bytes, uint32_t *serial)
{
	int win = range_of(addr);

	(void)bytes;
	if (win < 0 || range_taken[win])
		return 0U;
	range_taken[win] = 1U;
	range_serial[win] = next_serial++;
	*serial = range_serial[win];
	return 1U;
}

void zzapp_test_card_free(uint32_t card, uint32_t serial)
{
	int win = range_of(card);

	if (win >= 0 && range_taken[win] && range_serial[win] == serial)
		range_taken[win] = 0U;
}

uint32_t zzapp_test_hostheap_free(void)
{
	uint32_t win;
	uint32_t total = 0U;

	for (win = 0U; win < ZZAPP_SLOT_COUNT; win++) {
		if (!range_taken[win])
			total += ZZAPP_WINDOW_BYTES;
	}
	return total;
}

/* The commit ledger: a fixed capacity less what the live apps hold. */
#define COMMIT_CAPACITY (512U << 20)
#define APP_COMMIT (1U << 20)

uint32_t zzapp_loader_pool_free(void)
{
	uint32_t win;
	uint32_t used = 0U;

	for (win = 0U; win < ZZAPP_SLOT_COUNT; win++) {
		zzapp_rec_t *rec = window_rec(&windows[win]);

		if (rec)
			used += rec->proc.memory.committed;
	}
	return COMMIT_CAPACITY - used;
}

/* ---- Setup and requests -------------------------------------------------- */

static uint32_t next_req = 1U;

static void hook_nothing(void) { }
static int chunk_ok(uint32_t off, const void *src, uint32_t len)
{
	(void)off;
	(void)src;
	(void)len;
	return AAR_OK;
}
static int end_ok(void) { return AAR_OK; }

/* The loader refuses the image: its tables do not fit the reserve. It notes
 * the reason, as the real loader does, and fails the op. */
static int end_tables_refused(void)
{
	zzapp_services_note_refusal(AAR_REASON_TABLES);
	return AAR_E_NORESOURCE;
}

static void hooks_ok(void)
{
	service_test_loader_begin_hook = hook_nothing;
	service_test_loader_chunk_hook = chunk_ok;
	service_test_loader_end_hook = end_ok;
}

static void setup(uint32_t apps, uint32_t l2_total)
{
	uint32_t win;

	memset(range_taken, 0, sizeof(range_taken));
	next_serial = 1U;
	svc_env_init(apps, l2_total, NULL);
	zzapp_services_test_reset();
	for (win = 0U; win < ZZAPP_SLOT_COUNT; win++)
		zzapp_services_test_set_window_pool(win, win_mem[win]);
	zzapp_services_test_set_core(0);
	hooks_ok();
}

static void selector(uint8_t *req, uint32_t win, uint32_t gen)
{
	memset(req, 0, ZZAPP_MAILBOX_PAYLOAD_BYTES);
	zzapp_put_be32(req + ZZAPP_SEL_SLOT, win);
	zzapp_put_be32(req + ZZAPP_SEL_GENERATION, gen);
}

struct claim_result {
	uint16_t status;
	uint16_t reply_len;
	uint32_t win;
	uint32_t gen;
	uint32_t reason;
};

static struct claim_result claim(void)
{
	struct claim_result r = { 0 };
	uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };

	r.status = zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, req, sizeof(req),
					  reply, &r.reply_len, 1, next_req++, 0U);
	if (r.status == SDK_STATUS_OK) {
		r.win = zzapp_get_be32(reply + ZZAPP_ALLOC_SLOT);
		r.gen = zzapp_get_be32(reply + ZZAPP_ALLOC_GENERATION);
	} else if (r.reply_len == ZZAPP_ALLOC_REFUSAL_REPLY_BYTES) {
		r.reason = zzapp_get_be32(reply + ZZAPP_ALLOC_REASON);
	}
	return r;
}

static uint16_t simple_op(uint16_t opcode, uint32_t win, uint32_t gen)
{
	uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint16_t reply_len = 0U;

	selector(req, win, gen);
	return zzapp_mailbox_dispatch(opcode, req, sizeof(req), reply, &reply_len,
				      1, next_req++, 0U);
}

/* One deferred op, run by core 1 and harvested by core 0. Returns the status
 * the launcher is given. */
static uint16_t deferred_op(uint16_t opcode, uint32_t win, uint32_t gen)
{
	uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint16_t reply_len = 0U;
	uint16_t status;

	selector(req, win, gen);
	if (opcode == ZZAPP_OP_LOAD_CHUNK)
		zzapp_build_chunk_request(req, 0U, 1U);
	zzapp_put_be32(req + ZZAPP_SEL_SLOT, win);
	zzapp_put_be32(req + ZZAPP_SEL_GENERATION, gen);
	status = zzapp_mailbox_dispatch(opcode, req, sizeof(req), reply, &reply_len,
					1, next_req++, 0U);
	CHECK(status == SDK_STATUS_QUEUED);
	zzapp_services_test_set_core(1);
	zzapp_services_poll();
	if (opcode == ZZAPP_OP_START) {
		uint32_t argc;

		select_window(win);
		CHECK(zzapp_services_take_start(&argc));
		CHECK(zzapp_services_start_entered(gen) == AAR_OK);
	}
	zzapp_services_test_set_core(0);
	if (!zzapp_mailbox_harvest(NULL, NULL, NULL, &status, NULL, &reply_len)) {
		CHECK(0);
		return 0xffffU;
	}
	zzapp_mailbox_harvest_ack();
	return status;
}

/* Claimed, loaded and running; the loader's commit is played by the test. */
static void bring_up(uint32_t win, uint32_t gen)
{
	CHECK(deferred_op(ZZAPP_OP_LOAD_BEGIN, win, gen) == SDK_STATUS_OK);
	CHECK(WSTATE(win) == ZZAPP_ST_LOADING);
	CHECK(deferred_op(ZZAPP_OP_LOAD_CHUNK, win, gen) == SDK_STATUS_OK);
	CHECK(deferred_op(ZZAPP_OP_LOAD_END, win, gen) == SDK_STATUS_OK);
	window_rec(&windows[win])->proc.memory.committed = APP_COMMIT;
	CHECK(deferred_op(ZZAPP_OP_START, win, gen) == SDK_STATUS_OK);
	CHECK(WSTATE(win) == ZZAPP_ST_RUNNING);
}

/* The host's end of an app: terminal reason, stage 3 resources, retire. */
static void finish_app(uint32_t win, uint32_t reason)
{
	uint32_t pid;

	select_window(win);
	pid = zzapp_services_current_pid();
	zzapp_services_set_terminal(reason);
	zzapp_services_on_app_end();
	(void)zzapp_rec_retire(pid);
}

/* Use up record indices so the next admission is not index 0, and the window
 * number is not the pid. */
static void burn_records(uint32_t count)
{
	uint32_t i;

	for (i = 0U; i < count; i++) {
		uint32_t handle = 0U;
		zzapp_rec_t *rec = NULL;

		CHECK(zzapp_rec_admit(ZZAPP_REC_NO_WINDOW, &handle, &rec) == 0U);
		zzapp_rec_retire_tables(rec);
		CHECK(zzapp_rec_free(handle) == 0);
	}
}

/* ---- Counters ------------------------------------------------------------ */

struct resources {
	zzapp_rec_stats_t rec;
	zzapp_as_stats_t tables;
	uint32_t hostheap;
	uint32_t commit_free;
	uint32_t free_windows;
};

static void take(struct resources *s)
{
	uint32_t win;

	memset(s, 0, sizeof(*s));
	zzapp_rec_stats(&s->rec);
	zzapp_as_stats(&s->tables);
	s->hostheap = zzapp_test_hostheap_free();
	s->commit_free = zzapp_loader_pool_free();
	for (win = 0U; win < ZZAPP_SLOT_COUNT; win++) {
		if (WSTATE(win) == ZZAPP_ST_IDLE && windows[win].handle == 0U)
			s->free_windows++;
	}
}

static int resources_equal(const struct resources *a, const struct resources *b)
{
	return a->rec.free == b->rec.free && a->rec.claimed == b->rec.claimed &&
	       a->rec.live == b->rec.live && a->rec.retiring == b->rec.retiring &&
	       a->rec.reclaimed == b->rec.reclaimed &&
	       a->tables.l2_free == b->tables.l2_free &&
	       a->tables.l2_reserved == b->tables.l2_reserved &&
	       a->tables.l2_reserved_unused == b->tables.l2_reserved_unused &&
	       a->tables.l2_shared_free == b->tables.l2_shared_free &&
	       a->hostheap == b->hostheap && a->commit_free == b->commit_free &&
	       a->free_windows == b->free_windows;
}

static int tables_equal(const zzapp_rec_table_snapshot_t *a,
			const zzapp_rec_table_snapshot_t *b)
{
	return memcmp(a->gen, b->gen, sizeof(a->gen)) == 0 &&
	       memcmp(a->state, b->state, sizeof(a->state)) == 0 &&
	       memcmp(a->next, b->next, sizeof(a->next)) == 0 &&
	       a->head == b->head && a->tail == b->tail &&
	       a->free_count == b->free_count &&
	       a->owner_serial == b->owner_serial;
}

/* The lifecycle blocks a refused claim must leave alone: generation, state and
 * refusal words of every window that has memory. */
static uint32_t life_sum(void)
{
	uint32_t win;
	uint32_t sum = 0U;

	for (win = 0U; win < ZZAPP_SLOT_COUNT; win++) {
		if (!windows[win].window)
			continue;
		sum = sum * 31U + lget(&windows[win], ZZAPP_LIFE_GENERATION_OFF);
		sum = sum * 31U + lget(&windows[win], ZZAPP_LIFE_STATE_OFF);
		sum = sum * 31U + lget(&windows[win], ZZAPP_LIFE_REFUSAL_OFF);
	}
	return sum;
}

struct query {
	uint16_t len;
	uint32_t slots;
	uint32_t free_slots;
	uint32_t commit_free;
	uint32_t commit_total;
	uint32_t records_free;
	uint32_t tables_free;
	uint32_t tables_total;
	uint32_t hostheap_free;
};

static void query(struct query *q)
{
	uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };

	CHECK(zzapp_mailbox_dispatch(ZZAPP_OP_QUERY, req, sizeof(req), reply,
				     &q->len, 1, next_req++, 0U) == SDK_STATUS_OK);
	q->slots = zzapp_get_be32(reply + ZZAPP_QUERY_SLOTS);
	q->free_slots = zzapp_get_be32(reply + ZZAPP_QUERY_FREE_SLOTS);
	q->commit_free = zzapp_get_be32(reply + ZZAPP_QUERY_COMMIT_FREE);
	q->commit_total = zzapp_get_be32(reply + ZZAPP_QUERY_COMMIT_TOTAL);
	q->records_free = zzapp_get_be32(reply + ZZAPP_QUERY_RECORDS_FREE);
	q->tables_free = zzapp_get_be32(reply + ZZAPP_QUERY_TABLES_FREE);
	q->tables_total = zzapp_get_be32(reply + ZZAPP_QUERY_TABLES_TOTAL);
	q->hostheap_free = zzapp_get_be32(reply + ZZAPP_QUERY_HOSTHEAP_FREE);
}

static int query_equal(const struct query *a, const struct query *b)
{
	return a->free_slots == b->free_slots && a->commit_free == b->commit_free &&
	       a->commit_total == b->commit_total &&
	       a->records_free == b->records_free &&
	       a->tables_free == b->tables_free &&
	       a->tables_total == b->tables_total &&
	       a->hostheap_free == b->hostheap_free;
}

/* Claim every window of the count at once and stop them, so each holds its
 * window memory: that memory stays until the Amiga resets, and counts compared
 * afterwards are the ones a steady system returns to. */
static void warm_windows(uint32_t count)
{
	struct claim_result r[ZZAPP_SLOT_COUNT];
	uint32_t i;

	for (i = 0U; i < count; i++) {
		r[i] = claim();
		CHECK(r[i].status == SDK_STATUS_OK);
	}
	for (i = 0U; i < count; i++) {
		if (r[i].status == SDK_STATUS_OK)
			CHECK(simple_op(ZZAPP_OP_STOP, r[i].win, r[i].gen) ==
			      SDK_STATUS_OK);
	}
}

static uint32_t apps_for(uint32_t windows_needed)
{
	return windows_needed + 8U < ZZAPP_REC_MIN_APPS ? ZZAPP_REC_MIN_APPS :
	       windows_needed + 8U;
}

/* ---- Tests --------------------------------------------------------------- */

/* A refusal leaves the table bit for bit as it was, and holds nothing. */
struct before {
	struct resources res;
	zzapp_rec_table_snapshot_t tab;
	uint32_t life;
};

static void snap_before(struct before *b)
{
	take(&b->res);
	zzapp_rec_table_snapshot(&b->tab);
	b->life = life_sum();
}

static void expect_unchanged(const struct before *b, const char *what)
{
	struct resources res;
	zzapp_rec_table_snapshot_t tab;

	take(&res);
	zzapp_rec_table_snapshot(&tab);
	if (!resources_equal(&b->res, &res) || !tables_equal(&b->tab, &tab) ||
	    b->life != life_sum()) {
		printf("FAIL: %s left a counter changed\n", what);
		failures++;
	}
	checks++;
}

static void test_refusal_limit(void)
{
	struct before b;
	struct claim_result r;
	uint32_t i;

	setup(apps_for(ZZAPP_SLOT_COUNT), 0U);
	/* Window memory first, so the refusal is the only thing that changes. */
	warm_windows(ZZAPP_SLOT_COUNT);
	for (i = 0U; i < ZZAPP_SLOT_COUNT; i++) {
		r = claim();
		CHECK(r.status == SDK_STATUS_OK && r.win == i);
	}
	snap_before(&b);
	r = claim();
	CHECK(r.status == SDK_STATUS_BUSY);
	CHECK(r.reply_len == ZZAPP_ALLOC_REFUSAL_REPLY_BYTES);
	CHECK(r.reason == AAR_REASON_LIMIT);
	expect_unchanged(&b, "LIMIT");
	/* The refusal named the window ceiling, not records: records remain. */
	CHECK(b.res.rec.free >= 8U);
	/* A legacy claim is refused the same way, without a reply payload. */
	{
		uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
		uint16_t reply_len = 99U;

		CHECK(zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, NULL, 0U, reply,
					     &reply_len, 1, next_req++, 0U) ==
		      SDK_STATUS_BUSY);
		CHECK(reply_len == 0U);
	}
	expect_unchanged(&b, "legacy LIMIT");
}

static void test_refusal_memory(void)
{
	struct before b;
	struct claim_result r;
	struct claim_result again;
	uint32_t i;

	setup(8U, 0U);
	warm_windows(ZZAPP_SLOT_COUNT < 4U ? ZZAPP_SLOT_COUNT : 4U);
	/* Five of the eight records are in use elsewhere: the arena is down to
	 * three. */
	{
		uint32_t handle;
		zzapp_rec_t *rec;

		for (i = 0U; i < 5U; i++)
			CHECK(zzapp_rec_admit(ZZAPP_REC_NO_WINDOW, &handle, &rec) == 0U);
	}
	for (i = 0U; i < 3U; i++) {
		r = claim();
		CHECK(r.status == SDK_STATUS_OK);
	}
	snap_before(&b);
	r = claim();
	CHECK(r.status == SDK_STATUS_NO_MEMORY);
	CHECK(r.reply_len == ZZAPP_ALLOC_REFUSAL_REPLY_BYTES);
	CHECK(r.reason == AAR_REASON_MEMORY);
	expect_unchanged(&b, "MEMORY");
	/* The window it tried is not bound, and a record that comes back is
	 * enough for the next claim: nothing was left half held. */
	CHECK(windows[3].handle == 0U);
	CHECK(simple_op(ZZAPP_OP_STOP, 0U, windows[0].claim_generation) ==
	      SDK_STATUS_OK);
	again = claim();
	CHECK(again.status == SDK_STATUS_OK);
}

static void test_refusal_tables(void)
{
	struct before b;
	struct claim_result r;
	zzapp_as_stats_t tables;

	/* A pool for the kernel template and two reserves. */
	{
		zzapp_as_stats_t st;

		svc_env_init(8U, 0U, NULL);
		zzapp_as_stats(&st);
		setup(8U, st.l2_template + 2U * ZZAPP_AS_L2_RESERVE);
	}
	warm_windows(ZZAPP_SLOT_COUNT < 2U ? ZZAPP_SLOT_COUNT : 2U);
	r = claim();
	CHECK(r.status == SDK_STATUS_OK);
	r = claim();
	CHECK(r.status == SDK_STATUS_OK);
	snap_before(&b);
	r = claim();
	CHECK(r.status == SDK_STATUS_NO_MEMORY);
	CHECK(r.reply_len == ZZAPP_ALLOC_REFUSAL_REPLY_BYTES);
	CHECK(r.reason == AAR_REASON_TABLES);
	expect_unchanged(&b, "TABLES");
	zzapp_as_stats(&tables);
	CHECK(tables.l2_reserved == 2U * ZZAPP_AS_L2_RESERVE);
	CHECK(windows[2].handle == 0U);
	/* The record slot the refused claim popped is back: a freed reserve is
	 * enough. */
	CHECK(simple_op(ZZAPP_OP_STOP, 1U, windows[1].claim_generation) ==
	      SDK_STATUS_OK);
	r = claim();
	CHECK(r.status == SDK_STATUS_OK);
}

static void test_refusal_hostheap(void)
{
	struct before b;
	struct claim_result r;
	uint32_t serial = 0U;

	setup(apps_for(ZZAPP_SLOT_COUNT), 0U);
	/* Someone else holds window 0's range of the host heap. */
	CHECK(zzapp_test_card_alloc_at(ZZAPP_WINDOW_Z3_SLOT_ADDRESS(0U),
				       ZZAPP_WINDOW_BYTES, &serial) == 1U);
	snap_before(&b);
	r = claim();
	CHECK(r.status == SDK_STATUS_NO_MEMORY);
	CHECK(r.reply_len == ZZAPP_ALLOC_REFUSAL_REPLY_BYTES);
	CHECK(r.reason == AAR_REASON_HOSTHEAP);
	/* Admission had gone through the record and the reserve before the heap
	 * said no: all of it is undone, and the handle table is bit-identical. */
	expect_unchanged(&b, "HOSTHEAP");
	CHECK(windows[0].handle == 0U && windows[0].window == NULL);
	CHECK(lget(&windows[0], ZZAPP_LIFE_STATE_OFF) == 0U);
	zzapp_test_card_free(ZZAPP_WINDOW_Z3_SLOT_ADDRESS(0U), serial);
	r = claim();
	CHECK(r.status == SDK_STATUS_OK && r.win == 0U);
}

/* LOAD_BEGIN cannot get its staging block: the claim ends, everything the
 * claim held is back, and the lifecycle word says why. */
static void test_load_memory_refusal(void)
{
	struct before b;
	struct claim_result r;
	uint32_t pid;

	setup(apps_for(ZZAPP_SLOT_COUNT), 0U);
	warm_windows(1U);
	snap_before(&b);
	r = claim();
	CHECK(r.status == SDK_STATUS_OK);
	pid = PID_OF(r.win);
	service_test_loader_begin_rc = AAR_E_NOMEM;
	CHECK(deferred_op(ZZAPP_OP_LOAD_BEGIN, r.win, r.gen) == SDK_STATUS_NO_MEMORY);
	service_test_loader_begin_rc = AAR_OK;
	CHECK(lget(&windows[r.win], ZZAPP_LIFE_REFUSAL_OFF) == AAR_REASON_MEMORY);
	CHECK(WSTATE(r.win) == ZZAPP_ST_IDLE && windows[r.win].handle == 0U);
	CHECK(zzapp_rec_state(pid) == ZZAPP_REC_FREE);
	/* Windows, records, tables and commit are what they were before the
	 * claim; only the generation counters moved. */
	{
		struct resources res;

		take(&res);
		CHECK(resources_equal(&b.res, &res));
	}
	/* The next claim of the window starts clean. */
	r = claim();
	CHECK(r.status == SDK_STATUS_OK);
	CHECK(lget(&windows[r.win], ZZAPP_LIFE_REFUSAL_OFF) == AAR_REASON_NONE);
}

/* LOAD_END: the image needs more tables than the reserve holds. */
static void test_load_tables_refusal(void)
{
	struct before b;
	struct claim_result r;

	setup(apps_for(ZZAPP_SLOT_COUNT), 0U);
	warm_windows(1U);
	snap_before(&b);
	r = claim();
	CHECK(r.status == SDK_STATUS_OK);
	CHECK(deferred_op(ZZAPP_OP_LOAD_BEGIN, r.win, r.gen) == SDK_STATUS_OK);
	CHECK(deferred_op(ZZAPP_OP_LOAD_CHUNK, r.win, r.gen) == SDK_STATUS_OK);
	service_test_loader_end_hook = end_tables_refused;
	CHECK(deferred_op(ZZAPP_OP_LOAD_END, r.win, r.gen) == SDK_STATUS_NO_MEMORY);
	service_test_loader_end_hook = end_ok;
	CHECK(lget(&windows[r.win], ZZAPP_LIFE_REFUSAL_OFF) == AAR_REASON_TABLES);
	CHECK(WSTATE(r.win) == ZZAPP_ST_IDLE && windows[r.win].handle == 0U);
	{
		struct resources res;

		take(&res);
		CHECK(resources_equal(&b.res, &res));
	}
}

/* QUERY words 32 to 47, the corrected commit words, and the legacy form. */
static void test_query_words(void)
{
	struct query q0;
	struct query q1;
	struct query q2;
	struct claim_result r;
	uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint16_t len = 0U;
	zzapp_as_stats_t st;
	uint32_t apps = apps_for(ZZAPP_SLOT_COUNT);

	setup(apps, 0U);
	zzapp_as_stats(&st);
	query(&q0);
	CHECK(q0.len == ZZAPP_QUERY_EXT_REPLY_BYTES && q0.len == 48U);
	CHECK(q0.slots == ZZAPP_SLOT_COUNT);
	CHECK(q0.free_slots == ZZAPP_SLOT_COUNT);
	CHECK(q0.records_free == apps);
	CHECK(q0.tables_total == zzapp_rec_layout()->l2_total - st.l2_template);
	CHECK(q0.tables_free == q0.tables_total);
	CHECK(q0.hostheap_free == ZZAPP_SLOT_COUNT * ZZAPP_WINDOW_BYTES);
	/* COMMIT_FREE is what is still admissible, COMMIT_TOTAL the capacity. */
	CHECK(q0.commit_free == COMMIT_CAPACITY && q0.commit_total == COMMIT_CAPACITY);

	r = claim();
	CHECK(r.status == SDK_STATUS_OK);
	query(&q1);
	CHECK(q1.free_slots == q0.free_slots - 1U);
	CHECK(q1.records_free == q0.records_free - 1U);
	CHECK(q1.tables_free == q0.tables_free - ZZAPP_AS_L2_RESERVE);
	CHECK(q1.tables_total == q0.tables_total);
	CHECK(q1.hostheap_free == q0.hostheap_free - ZZAPP_WINDOW_BYTES);
	bring_up(r.win, r.gen);
	query(&q2);
	CHECK(q2.commit_free == COMMIT_CAPACITY - APP_COMMIT);
	CHECK(q2.commit_total == COMMIT_CAPACITY);
	/* The window memory stays after the app ends; the rest comes back. */
	finish_app(r.win, ZZAPP_END_EXIT);
	query(&q2);
	CHECK(q2.free_slots == q0.free_slots && q2.records_free == q0.records_free);
	CHECK(q2.tables_free == q0.tables_free && q2.commit_free == COMMIT_CAPACITY);
	CHECK(q2.hostheap_free == q1.hostheap_free);

	/* A legacy QUERY keeps its 16 bytes. */
	len = 0U;
	CHECK(zzapp_mailbox_dispatch(ZZAPP_OP_QUERY, NULL, 0U, reply, &len, 1,
				     next_req++, 0U) == SDK_STATUS_OK);
	CHECK(len == ZZAPP_QUERY_REPLY_BYTES);
	(void)req;
}

/* A window is not a record. Burn record indices, claim every window and check
 * that nothing assumed pid == window: each record knows its window, the app
 * thread of a record reaches its own window's memory, and a wait woken by the
 * service pass wakes the record's thread, not the thread of the same number. */
static void test_window_is_not_pid(void)
{
	uint32_t win;
	uint32_t apps = apps_for(ZZAPP_SLOT_COUNT);
	struct claim_result r[ZZAPP_SLOT_COUNT];
	uint32_t written;
	uint32_t target = ZZAPP_SLOT_COUNT > 2U ? 2U : ZZAPP_SLOT_COUNT - 1U;

	setup(apps, 0U);
	burn_records(1U);
	for (win = 0U; win < ZZAPP_SLOT_COUNT; win++) {
		r[win] = claim();
		CHECK(r[win].status == SDK_STATUS_OK && r[win].win == win);
	}
	for (win = 0U; win < ZZAPP_SLOT_COUNT; win++) {
		zzapp_rec_t *rec = window_rec(&windows[win]);

		CHECK(rec != NULL);
		CHECK(rec->hdr.window == win);
		CHECK(rec->hdr.pid != win);
		CHECK(zzapp_rec_by_handle(windows[win].handle) == rec);
	}
	CHECK(PID_OF(0U) != 0U);

	/* The thread of window 0's record writes window 0's console. */
	service_test_current_thread = (int)PID_OF(0U);
	zzapp_services_test_set_core(1);
	CHECK(zzapp_services_window() == win_mem[0]);
	CHECK(zzapp_svc_console_write("a", 1U, &written) == AAR_OK && written == 1U);
	CHECK(win_mem[0][ZZAPP_CONSOLE_OUT_OFF + ZZAPP_CONSOLE_HDR] == 'a');
	service_test_current_thread = -1;
	zzapp_services_test_set_core(0);
	CHECK(zzapp_get_be32(win_mem[1] + ZZAPP_CONSOLE_OUT_OFF) == 0U);

	/* A parked waiter of window `target` wakes through its own record. */
	RT(target).wait_kind = ZZAPP_WAIT_CONSOLE_READ;
	RT(target).wait_value = 0U;
	zzapp_put_be32(win_mem[target] + ZZAPP_CONSOLE_IN_OFF, 1U);
	TH(target).state = ZZAPP_THREAD_BLOCKED;
	{
		zzapp_rec_t *same_number = zzapp_rec_by_pid(target);

		same_number->thread.state = ZZAPP_THREAD_BLOCKED;
		zzapp_services_test_set_core(1);
		zzapp_services_poll();
		zzapp_services_test_set_core(0);
		CHECK(TH(target).state == ZZAPP_THREAD_RUNNABLE);
		CHECK(same_number->thread.state == ZZAPP_THREAD_BLOCKED);
	}
}

/* The wire generation is the window's counter, +1 per claim, whichever record
 * the claim got; the handle generation moves with the record's own life. A
 * request with a stale wire generation is refused as before. */
static void test_wire_generation_and_handles(void)
{
	struct claim_result first;
	struct claim_result second;
	struct claim_result third;
	uint32_t pid1;
	uint32_t handle1;

	setup(apps_for(ZZAPP_SLOT_COUNT), 0U);
	first = claim();
	CHECK(first.status == SDK_STATUS_OK && first.win == 0U);
	pid1 = PID_OF(0U);
	handle1 = windows[0].handle;
	CHECK(simple_op(ZZAPP_OP_STOP, 0U, first.gen) == SDK_STATUS_OK);

	/* Another window's claim takes the next record; window 0 comes back on
	 * a different record with the wire generation +1. */
	if (ZZAPP_SLOT_COUNT > 1U)
		windows[0].quarantined = 0;
	second = claim();
	CHECK(second.status == SDK_STATUS_OK && second.win == 0U);
	CHECK(second.gen == first.gen + 1U);
	CHECK(PID_OF(0U) != pid1);
	CHECK(zzapp_rec_by_handle(handle1) == NULL);
	CHECK(aar_handle_index(windows[0].handle) == PID_OF(0U));
	/* Stale wire generation: refused, and the live claim is untouched. */
	CHECK(simple_op(ZZAPP_OP_STATUS, 0U, first.gen) == SDK_STATUS_BAD_HANDLE);
	CHECK(simple_op(ZZAPP_OP_STOP, 0U, first.gen) == SDK_STATUS_BAD_HANDLE);
	CHECK(WSTATE(0U) == ZZAPP_ST_CLAIMED);
	CHECK(simple_op(ZZAPP_OP_STATUS, 0U, second.gen) == SDK_STATUS_OK);
	CHECK(simple_op(ZZAPP_OP_STOP, 0U, second.gen) == SDK_STATUS_OK);
	third = claim();
	CHECK(third.status == SDK_STATUS_OK && third.gen == second.gen + 1U);
	/* The first record's handle generation advanced once, by its own free,
	 * and has nothing to do with the three wire generations seen. */
	CHECK(aar_handle_generation(zzapp_rec_handle(pid1)) == 0U ||
	      zzapp_rec_by_handle(handle1) == NULL);
	CHECK(aar_handle_generation(handle1) == 1U);
}

/* ---- Many apps ----------------------------------------------------------- */

static void expect_back(uint32_t n, const struct query *base, const char *what)
{
	struct query q;
	uint32_t win;

	query(&q);
	if (!query_equal(&q, base)) {
		printf("FAIL: %s did not return to the QUERY counts (%u apps)\n",
		       what, (unsigned)n);
		failures++;
	}
	checks++;
	for (win = 0U; win < n; win++) {
		CHECK(windows[win].handle == 0U);
		CHECK(WSTATE(win) == ZZAPP_ST_IDLE);
	}
}

static void run_apps(uint32_t n)
{
	struct claim_result r[ZZAPP_SLOT_COUNT];
	struct query initial;
	struct query steady;
	zzapp_rec_stats_t stats;
	uint32_t win;
	uint32_t q;

	setup(apps_for(n), 0U);
	/* Make the record index differ from the window number. */
	burn_records(3U);
	query(&initial);
	warm_windows(n);
	query(&steady);
	CHECK(steady.hostheap_free == initial.hostheap_free - n * ZZAPP_WINDOW_BYTES);
	CHECK(steady.records_free == initial.records_free);

	/* Stop all: the launchers ask, the host ends each app by force. */
	for (win = 0U; win < n; win++) {
		r[win] = claim();
		CHECK(r[win].status == SDK_STATUS_OK && r[win].win == win);
		bring_up(win, r[win].gen);
	}
	{
		struct query q_live;

		query(&q_live);
		CHECK(q_live.records_free == steady.records_free - n);
		CHECK(q_live.tables_free == steady.tables_free - n * ZZAPP_AS_L2_RESERVE);
		CHECK(q_live.free_slots == steady.free_slots - n);
		CHECK(q_live.commit_free == steady.commit_free - n * APP_COMMIT);
		CHECK(q_live.commit_total == steady.commit_total);
	}
	zzapp_rec_stats(&stats);
	CHECK(stats.live == n);
	for (win = 0U; win < n; win++) {
		CHECK(simple_op(ZZAPP_OP_STOP, win, r[win].gen) == SDK_STATUS_OK);
		CHECK(zzapp_get_be32(win_mem[win] + ZZAPP_LIFE_STOP_OFF) == 1U);
		finish_app(win, ZZAPP_END_FORCED);
	}
	expect_back(n, &steady, "stop");

	/* Exit all. */
	for (win = 0U; win < n; win++) {
		r[win] = claim();
		CHECK(r[win].status == SDK_STATUS_OK && r[win].gen != 0U);
		bring_up(win, r[win].gen);
	}
	for (win = 0U; win < n; win++)
		finish_app(win, ZZAPP_END_EXIT);
	expect_back(n, &steady, "exit");
	{
		uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES];
		uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
		uint16_t len = 0U;

		selector(req, 0U, 0U);
		CHECK(zzapp_mailbox_dispatch(ZZAPP_OP_LIST, req, sizeof(req), reply,
					     &len, 1, next_req++, 0U) == SDK_STATUS_OK);
		CHECK(reply[ZZAPP_LIST_REASONS + 1U] == ZZAPP_END_EXIT);
	}

	/* Reset with every app live: the host retires each, then services. The
	 * window memory goes back too. */
	for (win = 0U; win < n; win++) {
		r[win] = claim();
		CHECK(r[win].status == SDK_STATUS_OK);
		bring_up(win, r[win].gen);
	}
	for (win = 0U; win < n; win++)
		finish_app(win, ZZAPP_END_AMIGA_RESET);
	zzapp_services_on_reset();
	expect_back(n, &initial, "reset");
	for (win = 0U; win < n; win++)
		CHECK(windows[win].window == NULL);

	/* Quarantine one claim that stopped talking while loading. */
	warm_windows(n);
	query(&steady);
	q = n / 2U;
	for (win = 0U; win < n; win++) {
		r[win] = claim();
		CHECK(r[win].status == SDK_STATUS_OK);
		if (win == q)
			CHECK(deferred_op(ZZAPP_OP_LOAD_BEGIN, win, r[win].gen) ==
			      SDK_STATUS_OK);
		else
			bring_up(win, r[win].gen);
	}
	zzapp_services_expire_claims(ZZAPP_LOAD_LEASE_US);
	CHECK(WSTATE(q) == ZZAPP_ST_STOPPING && windows[q].quarantined);
	{
		struct query before;
		struct query after;

		query(&before);
		select_window(q);
		zzapp_services_on_app_end();
		(void)zzapp_rec_retire(zzapp_services_current_pid());
		query(&after);
		/* Resources are back, the shell and handle are held. */
		CHECK(after.tables_free == before.tables_free + ZZAPP_AS_L2_RESERVE);
		CHECK(after.records_free == before.records_free);
		CHECK(windows[q].quarantine_reclaimed && windows[q].handle != 0U);
		CHECK(WSTATE(q) == ZZAPP_ST_STOPPING);
	}
	/* The launcher's next contact with its stale generation releases it. */
	{
		struct query released;

		CHECK(simple_op(ZZAPP_OP_LOAD_CHUNK, q, r[q].gen) == SDK_STATUS_BAD_HANDLE);
		query(&released);
		CHECK(released.records_free == steady.records_free - (n - 1U));
		CHECK(windows[q].handle == 0U && !windows[q].quarantined);
	}
	for (win = 0U; win < n; win++) {
		if (win != q)
			finish_app(win, ZZAPP_END_EXIT);
	}
	expect_back(n, &steady, "quarantine");
	zzapp_rec_stats(&stats);
	CHECK(stats.free == stats.capacity);
	CHECK(stats.claimed == 0U && stats.live == 0U && stats.retiring == 0U &&
	      stats.reclaimed == 0U);
}

int main(void)
{
	test_query_words();
	test_refusal_limit();
	test_refusal_memory();
	test_refusal_tables();
	test_refusal_hostheap();
	test_load_memory_refusal();
	test_load_tables_refusal();
	test_window_is_not_pid();
	test_wire_generation_and_handles();
	run_apps(ZZAPP_SLOT_COUNT < 8U ? ZZAPP_SLOT_COUNT : 8U);
	if (ZZAPP_SLOT_COUNT >= 64U)
		run_apps(64U);
	if (failures != 0) {
		printf("admission_test: %d of %d checks failed\n", failures, checks);
		return 1;
	}
	printf("admission_test: %d checks, 0 failures\n", checks);
	return 0;
}
