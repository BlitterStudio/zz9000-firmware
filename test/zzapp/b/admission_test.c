/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Claim admission over process records and the transport (slice 6).
 *
 * A claim takes, in order, a record slot (MEMORY), the L2 reserve (TABLES)
 * and the control block and queue pair from the host-visible heap (HOSTHEAP),
 * and publishes nothing until all three are held. Each refusal reason is
 * driven here with the resource it names exhausted, and must leave every
 * counter and the handle table as it was. Load-time refusals (staging,
 * tables) end the claim; the shell holds the pair until the client releases
 * it, and then everything is back. There is no ceiling of launcher windows
 * any more: many apps are live at once and every stop, exit, reset and
 * quarantine must return to the counts QUERY reports.
 *
 * The services run on the stub kernel and loader of
 * ZZAPP_SERVICES_STANDALONE_TEST, over the real record arena and table
 * layers and the mock host-visible heap. The host's retire path is the double
 * in svc_env.h (tables back, wire layer detached; the record and its pair
 * stay for the client).
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

/* ---- Host-heap model ----------------------------------------------------- */

#define HEAP_CAPACITY (64U << 20)

uint32_t zzapp_test_hostheap_free(void)
{
	return HEAP_CAPACITY - mock_heap.live_bytes;
}

uint32_t zzapp_test_hostheap_largest(void)
{
	return HEAP_CAPACITY - mock_heap.live_bytes;
}

/* The commit ledger: a fixed capacity less what the live apps hold. */
#define COMMIT_CAPACITY (512U << 20)
#define APP_COMMIT (1U << 20)

uint32_t zzapp_loader_pool_free(void)
{
	uint32_t pid;
	uint32_t used = 0U;

	for (pid = 0U; pid < zzapp_rec_capacity(); pid++) {
		zzapp_rec_t *rec = zzapp_rec_by_pid(pid);

		if (rec && zzapp_rec_state(pid) != ZZAPP_REC_FREE)
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
	svc_env_init(apps, l2_total, NULL);
	zzapp_services_test_reset();
	mock_heap.capacity = HEAP_CAPACITY;
	zzapp_services_test_set_core(0);
	hooks_ok();
}

struct claim_result {
	uint16_t status;
	uint16_t reply_len;
	uint32_t handle;
	uint32_t token;
	uint32_t bytes;
	uint32_t reason;
};

static struct claim_result claim(void)
{
	struct claim_result r = { 0 };
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };

	r.status = zzapp_mailbox_dispatch(ZZAPP_OP_CLAIM, NULL, 0U, reply,
					  &r.reply_len, 1, next_req++, 0U);
	if (r.status == SDK_STATUS_OK) {
		r.handle = zzapp_get_be32(reply + ZZAPP_CLAIM_HANDLE);
		r.token = zzapp_get_be32(reply + ZZAPP_CLAIM_CB_TOKEN);
		r.bytes = zzapp_get_be32(reply + ZZAPP_CLAIM_CB_BYTES);
	} else if (r.reply_len == ZZAPP_REFUSAL_REPLY_BYTES) {
		r.reason = zzapp_get_be32(reply + ZZAPP_REFUSAL_REASON);
	}
	return r;
}

static uint16_t simple_op(uint16_t opcode, uint32_t handle)
{
	uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint16_t reply_len = 0U;

	zzapp_build_handle(req, handle);
	return zzapp_mailbox_dispatch(opcode, req, sizeof(req), reply, &reply_len,
				      1, next_req++, 0U);
}

/* One deferred op, run by core 1 and harvested by core 0. Returns the status
 * the client is given; the reply payload when asked. */
static uint16_t deferred_op_reply(uint16_t opcode, uint32_t handle,
				  uint8_t *payload, uint16_t *payload_len)
{
	uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint16_t reply_len = 0U;
	uint16_t status;
	zzapp_rec_t *rec = zzapp_rec_by_handle(handle);

	if (opcode == ZZAPP_OP_LOAD_CHUNK)
		zzapp_build_load_chunk_request(req, handle, 0U, 1U);
	else if (opcode == ZZAPP_OP_START)
		zzapp_build_start_app_request(req, handle, 0U);
	else if (opcode == ZZAPP_OP_LOAD_BEGIN)
		zzapp_build_load_begin_request(req, handle, 0U);
	else
		zzapp_build_handle(req, handle);
	status = zzapp_mailbox_dispatch(opcode, req, sizeof(req), reply, &reply_len,
					1, next_req++, 0U);
	CHECK(status == SDK_STATUS_QUEUED);
	zzapp_services_test_set_core(1);
	zzapp_services_poll();
	if (opcode == ZZAPP_OP_START && rec) {
		uint32_t argc;

		zzapp_services_select_pid(rec->hdr.pid);
		CHECK(zzapp_services_take_start(&argc));
		CHECK(zzapp_services_start_entered(zzapp_services_generation()) == AAR_OK);
	}
	zzapp_services_test_set_core(0);
	reply_len = 0U;
	if (!zzapp_mailbox_harvest(NULL, NULL, NULL, &status, payload ? payload : reply,
				   &reply_len)) {
		CHECK(0);
		return 0xffffU;
	}
	if (payload_len)
		*payload_len = reply_len;
	zzapp_mailbox_harvest_ack();
	return status;
}

static uint16_t deferred_op(uint16_t opcode, uint32_t handle)
{
	return deferred_op_reply(opcode, handle, NULL, NULL);
}

/* Claimed, loaded and running; the loader's commit is played by the test. */
static void bring_up(uint32_t handle)
{
	zzapp_rec_t *rec = zzapp_rec_by_handle(handle);

	CHECK(deferred_op(ZZAPP_OP_LOAD_BEGIN, handle) == SDK_STATUS_OK);
	CHECK(rec && rec->claim.state == AAR_STATE_LOADING);
	CHECK(deferred_op(ZZAPP_OP_LOAD_CHUNK, handle) == SDK_STATUS_OK);
	CHECK(deferred_op(ZZAPP_OP_LOAD_END, handle) == SDK_STATUS_OK);
	if (rec)
		rec->proc.memory.committed = APP_COMMIT;
	CHECK(deferred_op(ZZAPP_OP_START, handle) == SDK_STATUS_OK);
	CHECK(rec && rec->claim.state == AAR_STATE_RUNNING);
}

/* The host's end of an app: terminal reason, stage 3 resources, retire. The
 * record is a shell now and holds its pair until the client releases it. */
static void finish_app(uint32_t handle, uint32_t reason)
{
	zzapp_rec_t *rec = zzapp_rec_by_handle(handle);
	uint32_t pid;

	CHECK(rec != NULL);
	if (!rec)
		return;
	pid = rec->hdr.pid;
	zzapp_services_select_pid(pid);
	zzapp_services_set_terminal(reason);
	zzapp_services_on_app_end();
	/* The host's retire returns the frames, and the commit with them. */
	rec->proc.memory.committed = 0U;
	(void)zzapp_rec_retire(pid);
}

/* Use up record indices so the next admission is not index 0. */
static void burn_records(uint32_t count)
{
	uint32_t i;

	for (i = 0U; i < count; i++) {
		uint32_t handle = 0U;
		zzapp_rec_t *rec = NULL;

		CHECK(zzapp_rec_admit(&handle, &rec) == 0U);
		zzapp_rec_retire_tables(rec);
		CHECK(zzapp_rec_free(handle) == 0);
	}
}

/* ---- Counters ------------------------------------------------------------ */

struct resources {
	zzapp_rec_stats_t rec;
	zzapp_as_stats_t tables;
	uint32_t hostheap;
	uint32_t hostheap_blocks;
	uint32_t commit_free;
};

static void take(struct resources *s)
{
	memset(s, 0, sizeof(*s));
	zzapp_rec_stats(&s->rec);
	zzapp_as_stats(&s->tables);
	s->hostheap = zzapp_test_hostheap_free();
	s->hostheap_blocks = mock_heap.live;
	s->commit_free = zzapp_loader_pool_free();
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
	       a->hostheap == b->hostheap &&
	       a->hostheap_blocks == b->hostheap_blocks &&
	       a->commit_free == b->commit_free;
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

/* The lifecycle state of every record a refused claim must leave alone. */
static uint32_t life_sum(void)
{
	uint32_t pid;
	uint32_t sum = 0U;

	for (pid = 0U; pid < zzapp_rec_capacity(); pid++) {
		zzapp_rec_t *rec = zzapp_rec_by_pid(pid);

		if (!rec || zzapp_rec_state(pid) == ZZAPP_REC_FREE)
			continue;
		sum = sum * 31U + rec->claim.state;
		sum = sum * 31U + zzapp_xport_cb_get(rec, AAR_CB_STATE);
		sum = sum * 31U + zzapp_xport_cb_get(rec, AAR_CB_END_REASON);
		sum = sum * 31U + zzapp_xport_shared_used(rec);
	}
	return sum;
}

struct query {
	uint16_t len;
	uint32_t depth;
	uint32_t hostheap_largest;
	uint32_t commit_free;
	uint32_t commit_total;
	uint32_t records_free;
	uint32_t tables_free;
	uint32_t tables_total;
	uint32_t hostheap_free;
};

static void query(struct query *q)
{
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };

	CHECK(zzapp_mailbox_dispatch(ZZAPP_OP_QUERY, NULL, 0U, reply, &q->len, 1,
				     next_req++, 0U) == SDK_STATUS_OK);
	q->depth = zzapp_get_be32(reply + ZZAPP_QUERY_QUEUE_DEPTH);
	q->hostheap_largest = zzapp_get_be32(reply + ZZAPP_QUERY_HOSTHEAP_LARGEST);
	q->commit_free = zzapp_get_be32(reply + ZZAPP_QUERY_COMMIT_FREE);
	q->commit_total = zzapp_get_be32(reply + ZZAPP_QUERY_COMMIT_TOTAL);
	q->records_free = zzapp_get_be32(reply + ZZAPP_QUERY_RECORDS_FREE);
	q->tables_free = zzapp_get_be32(reply + ZZAPP_QUERY_TABLES_FREE);
	q->tables_total = zzapp_get_be32(reply + ZZAPP_QUERY_TABLES_TOTAL);
	q->hostheap_free = zzapp_get_be32(reply + ZZAPP_QUERY_HOSTHEAP_FREE);
}

static int query_equal(const struct query *a, const struct query *b)
{
	return a->commit_free == b->commit_free &&
	       a->commit_total == b->commit_total &&
	       a->records_free == b->records_free &&
	       a->tables_free == b->tables_free &&
	       a->tables_total == b->tables_total &&
	       a->hostheap_free == b->hostheap_free &&
	       a->hostheap_largest == b->hostheap_largest;
}

static uint32_t apps_for(uint32_t needed)
{
	return needed + 8U < ZZAPP_REC_MIN_APPS ? ZZAPP_REC_MIN_APPS : needed + 8U;
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

static void test_refusal_memory(void)
{
	struct before b;
	struct claim_result r[4];
	struct claim_result again;
	uint32_t i;

	setup(8U, 0U);
	/* Five of the eight records are in use elsewhere: the arena is down to
	 * three. */
	{
		uint32_t handle;
		zzapp_rec_t *rec;

		for (i = 0U; i < 5U; i++)
			CHECK(zzapp_rec_admit(&handle, &rec) == 0U);
	}
	for (i = 0U; i < 3U; i++) {
		r[i] = claim();
		CHECK(r[i].status == SDK_STATUS_OK);
	}
	snap_before(&b);
	r[3] = claim();
	CHECK(r[3].status == SDK_STATUS_NO_MEMORY);
	CHECK(r[3].reply_len == ZZAPP_REFUSAL_REPLY_BYTES);
	CHECK(r[3].reason == AAR_REASON_MEMORY);
	expect_unchanged(&b, "MEMORY");
	/* Nothing was left half held: a record that comes back is enough. */
	CHECK(simple_op(ZZAPP_OP_RELEASE, r[0].handle) == SDK_STATUS_OK);
	again = claim();
	CHECK(again.status == SDK_STATUS_OK);
}

static void test_refusal_tables(void)
{
	struct before b;
	struct claim_result r[3];
	struct claim_result again;
	zzapp_as_stats_t tables;

	/* A pool for the kernel template and two reserves. */
	{
		zzapp_as_stats_t st;

		svc_env_init(8U, 0U, NULL);
		zzapp_as_stats(&st);
		setup(8U, st.l2_template + 2U * ZZAPP_AS_L2_RESERVE);
	}
	r[0] = claim();
	CHECK(r[0].status == SDK_STATUS_OK);
	r[1] = claim();
	CHECK(r[1].status == SDK_STATUS_OK);
	snap_before(&b);
	r[2] = claim();
	CHECK(r[2].status == SDK_STATUS_NO_MEMORY);
	CHECK(r[2].reply_len == ZZAPP_REFUSAL_REPLY_BYTES);
	CHECK(r[2].reason == AAR_REASON_TABLES);
	expect_unchanged(&b, "TABLES");
	zzapp_as_stats(&tables);
	CHECK(tables.l2_reserved == 2U * ZZAPP_AS_L2_RESERVE);
	/* The record the refused claim popped is back: a freed reserve is
	 * enough. */
	CHECK(simple_op(ZZAPP_OP_RELEASE, r[1].handle) == SDK_STATUS_OK);
	again = claim();
	CHECK(again.status == SDK_STATUS_OK);
}

static void test_refusal_hostheap(void)
{
	struct before b;
	struct claim_result r;

	setup(apps_for(8U), 0U);
	/* The host-visible heap has no room for a 16 KiB pair. */
	mock_heap.fail_after = 0;
	snap_before(&b);
	r = claim();
	CHECK(r.status == SDK_STATUS_NO_MEMORY);
	CHECK(r.reply_len == ZZAPP_REFUSAL_REPLY_BYTES);
	CHECK(r.reason == AAR_REASON_HOSTHEAP);
	/* Admission had gone through the record and the reserve before the heap
	 * said no: all of it is undone, and the handle table is bit-identical. */
	expect_unchanged(&b, "HOSTHEAP");
	CHECK(mock_heap.live == 0U);
	mock_heap.fail_after = -1;

	/* The same when the heap holds fewer bytes than the pair. */
	mock_heap.capacity = 0x3000U;
	snap_before(&b);
	r = claim();
	CHECK(r.status == SDK_STATUS_NO_MEMORY && r.reason == AAR_REASON_HOSTHEAP);
	expect_unchanged(&b, "HOSTHEAP capacity");
	mock_heap.capacity = HEAP_CAPACITY;
	r = claim();
	CHECK(r.status == SDK_STATUS_OK && r.bytes == AAR_PAIR_BYTES);
	/* The pair is a 16 KiB block aligned to 8 KiB. */
	CHECK(mock_heap.last_bytes == ZZAPP_XPORT_PAIR_ALLOC &&
	      mock_heap.last_align == ZZAPP_XPORT_PAIR_ALIGN);
}

/* LOAD_BEGIN cannot get its staging block: the claim ends with the reason in
 * the reply, the shell holds its pair, and a release gives everything back. */
static void test_load_memory_refusal(void)
{
	struct before b;
	struct claim_result r;
	uint8_t payload[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint16_t plen = 0U;
	zzapp_rec_t *rec;

	setup(apps_for(8U), 0U);
	snap_before(&b);
	r = claim();
	CHECK(r.status == SDK_STATUS_OK);
	rec = zzapp_rec_by_handle(r.handle);
	CHECK(rec != NULL);
	service_test_loader_begin_rc = AAR_E_NOMEM;
	CHECK(deferred_op_reply(ZZAPP_OP_LOAD_BEGIN, r.handle, payload, &plen) ==
	      SDK_STATUS_NO_MEMORY);
	service_test_loader_begin_rc = AAR_OK;
	CHECK(plen == ZZAPP_REFUSAL_REPLY_BYTES &&
	      zzapp_get_be32(payload + ZZAPP_REFUSAL_REASON) == AAR_REASON_MEMORY);
	if (rec) {
		CHECK(zzapp_xport_cb_get(rec, AAR_CB_STATE) == AAR_STATE_IDLE);
		CHECK(zzapp_rec_state(rec->hdr.pid) == ZZAPP_REC_RECLAIMED);
	}
	/* Tables and commit are back at once; the record and the pair go with
	 * the release. */
	CHECK(simple_op(ZZAPP_OP_RELEASE, r.handle) == SDK_STATUS_OK);
	{
		struct resources res;

		take(&res);
		CHECK(resources_equal(&b.res, &res));
	}
	/* The next claim starts clean. */
	r = claim();
	CHECK(r.status == SDK_STATUS_OK);
	rec = zzapp_rec_by_handle(r.handle);
	CHECK(rec && zzapp_xport_cb_get(rec, AAR_CB_STATE) == AAR_STATE_CLAIMED);
}

/* LOAD_END: the image needs more tables than the reserve holds. */
static void test_load_tables_refusal(void)
{
	struct before b;
	struct claim_result r;
	uint8_t payload[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
	uint16_t plen = 0U;

	setup(apps_for(8U), 0U);
	snap_before(&b);
	r = claim();
	CHECK(r.status == SDK_STATUS_OK);
	CHECK(deferred_op(ZZAPP_OP_LOAD_BEGIN, r.handle) == SDK_STATUS_OK);
	CHECK(deferred_op(ZZAPP_OP_LOAD_CHUNK, r.handle) == SDK_STATUS_OK);
	service_test_loader_end_hook = end_tables_refused;
	CHECK(deferred_op_reply(ZZAPP_OP_LOAD_END, r.handle, payload, &plen) ==
	      SDK_STATUS_NO_MEMORY);
	service_test_loader_end_hook = end_ok;
	CHECK(plen == ZZAPP_REFUSAL_REPLY_BYTES &&
	      zzapp_get_be32(payload + ZZAPP_REFUSAL_REASON) == AAR_REASON_TABLES);
	CHECK(simple_op(ZZAPP_OP_RELEASE, r.handle) == SDK_STATUS_OK);
	{
		struct resources res;

		take(&res);
		CHECK(resources_equal(&b.res, &res));
	}
}

/* QUERY: the counts a claim, load and release cycle returns to. */
static void test_query_words(void)
{
	struct query q0;
	struct query q1;
	struct query q2;
	struct query q3;
	struct claim_result r;
	zzapp_as_stats_t st;
	uint32_t apps = apps_for(8U);

	setup(apps, 0U);
	zzapp_as_stats(&st);
	query(&q0);
	CHECK(q0.len == ZZAPP_MAILBOX_PAYLOAD_BYTES);
	CHECK(q0.depth == AAR_QUEUE_DEPTH_DEFAULT);
	CHECK(q0.records_free == apps);
	CHECK(q0.tables_total == zzapp_rec_layout()->l2_total - st.l2_template);
	CHECK(q0.tables_free == q0.tables_total);
	CHECK(q0.hostheap_free == HEAP_CAPACITY);
	CHECK(q0.hostheap_largest == HEAP_CAPACITY);
	/* COMMIT_FREE is what is still admissible, COMMIT_TOTAL the capacity. */
	CHECK(q0.commit_free == COMMIT_CAPACITY && q0.commit_total == COMMIT_CAPACITY);

	r = claim();
	CHECK(r.status == SDK_STATUS_OK);
	CHECK(r.bytes == AAR_PAIR_BYTES);
	query(&q1);
	CHECK(q1.records_free == q0.records_free - 1U);
	CHECK(q1.tables_free == q0.tables_free - ZZAPP_AS_L2_RESERVE);
	CHECK(q1.tables_total == q0.tables_total);
	CHECK(q1.hostheap_free == q0.hostheap_free - ZZAPP_XPORT_PAIR_ALLOC);
	bring_up(r.handle);
	query(&q2);
	CHECK(q2.commit_free == COMMIT_CAPACITY - APP_COMMIT);
	CHECK(q2.commit_total == COMMIT_CAPACITY);
	/* The load buffer is a block of the claim too. */
	CHECK(q2.hostheap_free <= q1.hostheap_free);
	/* The app ends: tables and commit are back, the shell holds the rest
	 * until the client lets go. */
	finish_app(r.handle, ZZAPP_END_EXIT);
	query(&q3);
	CHECK(q3.tables_free == q0.tables_free && q3.commit_free == COMMIT_CAPACITY);
	CHECK(q3.records_free == q0.records_free - 1U);
	CHECK(q3.hostheap_free <= q1.hostheap_free);
	CHECK(simple_op(ZZAPP_OP_RELEASE, r.handle) == SDK_STATUS_OK);
	query(&q3);
	CHECK(query_equal(&q3, &q0));
}

/* An app is not its record index. Burn record indices, claim many apps and
 * check that nothing assumed pid == app number: each client reads its own
 * pair, the thread of a record reaches its own console, and a wait woken by
 * the service pass wakes the record's thread, not the thread of the same
 * number. */
static void test_app_is_not_pid(void)
{
	uint32_t i;
	enum { N = 6 };
	struct claim_result r[N];
	zzapp_rec_t *rec[N];
	mock_client cl[N];
	uint32_t written;

	setup(apps_for(N), 0U);
	burn_records(1U);
	for (i = 0U; i < N; i++) {
		r[i] = claim();
		CHECK(r[i].status == SDK_STATUS_OK);
		rec[i] = zzapp_rec_by_handle(r[i].handle);
		CHECK(rec[i] != NULL);
		mock_client_init(&cl[i]);
		CHECK(mock_client_attach(&cl[i], r[i].token) == AAR_OK);
		CHECK(cl[i].handle == r[i].handle);
		CHECK(aar_handle_index(r[i].handle) == rec[i]->hdr.pid);
		(void)zzapp_as_create(&rec[i]->proc.as, rec[i]->hdr.pid);
	}
	CHECK(rec[0]->hdr.pid != 0U);
	/* Each pair is its own block. */
	for (i = 1U; i < N; i++)
		CHECK(r[i].token != r[0].token);

	/* The thread of record 0 writes record 0's console. */
	service_test_current_thread = (int)rec[0]->hdr.pid;
	zzapp_services_test_set_core(1);
	zzapp_services_test_set_time(NULL);
	CHECK(zzapp_svc_console_write("a", 1U, &written) == AAR_OK && written == 1U);
	service_test_current_thread = -1;
	zzapp_services_select_pid(rec[0]->hdr.pid);
	(void)mock_client_serve(&cl[0]);
	zzapp_services_poll();
	(void)mock_client_serve(&cl[0]);
	CHECK(cl[0].console_out_len == 1U && cl[0].console_out[0] == 'a');
	CHECK(mock_client_sq_used(&cl[1]) == 0);
}

/* A handle is the record index and a generation. A released claim's handle is
 * stale for good, the live claim is untouched, and the next claim of the
 * record has a new generation. */
static void test_handles_after_reuse(void)
{
	struct claim_result first;
	struct claim_result second;
	struct claim_result third;
	uint32_t pid1;

	setup(apps_for(8U), 0U);
	first = claim();
	CHECK(first.status == SDK_STATUS_OK);
	pid1 = aar_handle_index(first.handle);
	CHECK(simple_op(ZZAPP_OP_STOP, first.handle) == SDK_STATUS_OK);
	CHECK(simple_op(ZZAPP_OP_RELEASE, first.handle) == SDK_STATUS_OK);

	second = claim();
	CHECK(second.status == SDK_STATUS_OK);
	CHECK(second.handle != first.handle);
	CHECK(zzapp_rec_by_handle(first.handle) == NULL);
	/* Stale: refused, and the live claim is untouched. */
	CHECK(simple_op(ZZAPP_OP_STOP, first.handle) == SDK_STATUS_BAD_HANDLE);
	CHECK(simple_op(ZZAPP_OP_RELEASE, first.handle) == SDK_STATUS_BAD_HANDLE);
	CHECK(zzapp_rec_by_handle(second.handle) != NULL);
	CHECK(zzapp_rec_by_handle(second.handle)->claim.state == AAR_STATE_CLAIMED);
	CHECK(simple_op(ZZAPP_OP_STOP, second.handle) == SDK_STATUS_OK);
	CHECK(simple_op(ZZAPP_OP_RELEASE, second.handle) == SDK_STATUS_OK);
	third = claim();
	CHECK(third.status == SDK_STATUS_OK && third.handle != second.handle);
	/* The first record's generation advanced once, by its own free. */
	CHECK(aar_handle_generation(zzapp_rec_handle(pid1)) == 0U ||
	      zzapp_rec_by_handle(first.handle) == NULL);
	CHECK(aar_handle_generation(first.handle) == 1U);
}

/* ---- Many apps ----------------------------------------------------------- */

static void expect_back(const struct query *base, const char *what)
{
	struct query q;

	query(&q);
	if (!query_equal(&q, base)) {
		printf("FAIL: %s did not return to the QUERY counts\n", what);
		failures++;
	}
	checks++;
}

static void run_apps(uint32_t n)
{
	struct claim_result *r = calloc(n, sizeof(*r));
	struct query initial;
	zzapp_rec_stats_t stats;
	uint32_t i;
	uint32_t q;

	setup(apps_for(n), 0U);
	/* Make the record index differ from the app number. */
	burn_records(3U);
	query(&initial);

	/* Claim and bring up n apps: past four, with no window ceiling. */
	for (i = 0U; i < n; i++) {
		r[i] = claim();
		CHECK(r[i].status == SDK_STATUS_OK);
		bring_up(r[i].handle);
	}
	{
		struct query q_live;

		query(&q_live);
		CHECK(q_live.records_free == initial.records_free - n);
		CHECK(q_live.tables_free == initial.tables_free - n * ZZAPP_AS_L2_RESERVE);
		CHECK(q_live.commit_free == initial.commit_free - n * APP_COMMIT);
		CHECK(q_live.commit_total == initial.commit_total);
	}
	zzapp_rec_stats(&stats);
	CHECK(stats.live == n);

	/* Stop all: the clients ask, the host ends each app by force. */
	for (i = 0U; i < n; i++) {
		CHECK(simple_op(ZZAPP_OP_STOP, r[i].handle) == SDK_STATUS_OK);
		zzapp_services_select_pid(aar_handle_index(r[i].handle));
		CHECK(zzapp_services_stop_requested());
		finish_app(r[i].handle, ZZAPP_END_FORCED);
		CHECK(simple_op(ZZAPP_OP_RELEASE, r[i].handle) == SDK_STATUS_OK);
	}
	expect_back(&initial, "stop");

	/* Exit all: the client reads the end from its control block before it
	 * releases. */
	for (i = 0U; i < n; i++) {
		r[i] = claim();
		CHECK(r[i].status == SDK_STATUS_OK && r[i].handle != 0U);
		bring_up(r[i].handle);
	}
	for (i = 0U; i < n; i++)
		finish_app(r[i].handle, ZZAPP_END_EXIT);
	for (i = 0U; i < n; i++) {
		zzapp_rec_t *rec = zzapp_rec_by_handle(r[i].handle);

		CHECK(rec != NULL);
		if (rec) {
			CHECK(zzapp_xport_cb_get(rec, AAR_CB_END_REASON) == AAR_END_EXIT);
			CHECK(zzapp_xport_cb_get(rec, AAR_CB_STATE) == AAR_STATE_IDLE);
		}
	}
	{
		uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
		uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
		uint16_t len = 0U;

		/* LIST by index finds a shell by its end reason too. */
		zzapp_build_list_request(req, aar_handle_index(r[0].handle));
		CHECK(zzapp_mailbox_dispatch(ZZAPP_OP_LIST, req, sizeof(req), reply,
					     &len, 1, next_req++, 0U) == SDK_STATUS_OK);
		CHECK(zzapp_get_be32(reply + ZZAPP_LIST_HANDLE) == r[0].handle);
		CHECK(reply[ZZAPP_LIST_REASONS + 1U] == ZZAPP_END_EXIT);
	}
	for (i = 0U; i < n; i++)
		CHECK(simple_op(ZZAPP_OP_RELEASE, r[i].handle) == SDK_STATUS_OK);
	expect_back(&initial, "exit");

	/* Reset with every app live: the host retires each, then services. The
	 * shells and their pairs go back too. */
	for (i = 0U; i < n; i++) {
		r[i] = claim();
		CHECK(r[i].status == SDK_STATUS_OK);
		bring_up(r[i].handle);
	}
	for (i = 0U; i < n; i++)
		finish_app(r[i].handle, ZZAPP_END_AMIGA_RESET);
	zzapp_services_on_reset();
	expect_back(&initial, "reset");
	for (i = 0U; i < n; i++)
		CHECK(zzapp_rec_by_handle(r[i].handle) == NULL);
	CHECK(mock_heap.live == 0U);

	/* Quarantine one claim that stopped talking while loading. */
	q = n / 2U;
	for (i = 0U; i < n; i++) {
		r[i] = claim();
		CHECK(r[i].status == SDK_STATUS_OK);
		if (i == q)
			CHECK(deferred_op(ZZAPP_OP_LOAD_BEGIN, r[i].handle) == SDK_STATUS_OK);
		else
			bring_up(r[i].handle);
	}
	zzapp_services_expire_claims(ZZAPP_LOAD_LEASE_US);
	{
		zzapp_rec_t *rec = zzapp_rec_by_pid(aar_handle_index(r[q].handle));
		zzapp_xport_buf_t load;
		struct query before;
		struct query after;
		uint32_t current;

		CHECK(rec != NULL && rec->claim.state == AAR_STATE_STOPPING);
		current = rec ? rec->hdr.handle : 0U;
		CHECK(current != 0U && current != r[q].handle);
		CHECK(rec && (zzapp_xport_flag_test(rec, ZZAPP_XPORT_F_QUARANTINED) != 0));
		/* The claim was loading: its load buffer is the one block the end
		 * of the app gives back (design 3.9); the pair stays. */
		CHECK(rec && zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_LOAD, &load) == 1);
		query(&before);
		zzapp_services_select_pid(aar_handle_index(r[q].handle));
		zzapp_services_on_app_end();
		(void)zzapp_rec_retire(aar_handle_index(r[q].handle));
		query(&after);
		/* Resources are back, the shell and its pair are held. */
		CHECK(after.tables_free == before.tables_free + ZZAPP_AS_L2_RESERVE);
		CHECK(after.records_free == before.records_free);
		CHECK(after.hostheap_free == before.hostheap_free + load.bytes);
		CHECK(rec && zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_LOAD, &load) == 0);
		CHECK(rec && zzapp_xport_live(rec));
	}
	/* The client's next contact with its stale handle releases the shell. */
	{
		struct query released;
		struct query live;

		query(&live);
		CHECK(simple_op(ZZAPP_OP_LOAD_CHUNK, r[q].handle) == SDK_STATUS_BAD_HANDLE);
		query(&released);
		CHECK(released.records_free == live.records_free + 1U);
		CHECK(released.hostheap_free > live.hostheap_free);
	}
	for (i = 0U; i < n; i++) {
		if (i != q) {
			finish_app(r[i].handle, ZZAPP_END_EXIT);
			CHECK(simple_op(ZZAPP_OP_RELEASE, r[i].handle) == SDK_STATUS_OK);
		}
	}
	expect_back(&initial, "quarantine");
	zzapp_rec_stats(&stats);
	CHECK(stats.free == stats.capacity - 0U);
	CHECK(stats.claimed == 0U && stats.live == 0U && stats.retiring == 0U &&
	      stats.reclaimed == 0U);
	free(r);
}

int main(void)
{
	test_query_words();
	test_refusal_memory();
	test_refusal_tables();
	test_refusal_hostheap();
	test_load_memory_refusal();
	test_load_tables_refusal();
	test_app_is_not_pid();
	test_handles_after_reuse();
	run_apps(8U);
	run_apps(64U);
	run_apps(200U);
	if (failures != 0) {
		printf("admission_test: %d of %d checks failed\n", failures, checks);
		return 1;
	}
	printf("admission_test: %d checks, 0 failures\n", checks);
	return 0;
}
