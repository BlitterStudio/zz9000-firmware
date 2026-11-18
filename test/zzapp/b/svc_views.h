/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Apps and their clients for suites that include zzapp_services.c. An app is
 * a process record with its queue pair (zzapp_xport_admit) and a model of the
 * Amiga client (mock_client) bound to that pair, numbered 0 to SVC_MAX_APPS-1
 * by the suite. There are no window slots: the number is only the suite's own
 * index into g_app[]. Include after zzapp_services.c and svc_env.h.
 *
 * This header is the one place where the suites touch the services' internals
 * (the services part of the record, the deferred operation of a record and
 * the record's thread). Everything else goes through the public entry points
 * and the transport accessors.
 *
 * Interface assumed of zzapp_services.c in the standalone test build:
 *   zzapp_service_runtime: defer (.state, .opcode, .cancelled), audio_*,
 *   display_*, wait_kind/wait_value/wait_deadline; DEFER_*; run_deferred(service_rec());
 *   service_rec(), service_pid(), service_proc(), current_runtime(),
 *   runtime_of(rec); audio_detach(); the service_test_* hooks.
 */
#ifndef B_SVC_VIEWS_H
#define B_SVC_VIEWS_H

#include "mock_client.h"

#define SVC_MAX_APPS 80u

typedef struct svc_app {
	mock_client c;
	zzapp_rec_t *rec;
	uint32_t handle;
	uint32_t pid;
} svc_app;

static svc_app g_app[SVC_MAX_APPS];
static uint32_t svc_request_id;

#define APP(n)  (g_app[(n)])
#define CL(n)   (g_app[(n)].c)
#define RT(n)   (*runtime_of(g_app[(n)].rec))
#define TH(n)   (g_app[(n)].rec->thread)
#define DEF(n)  (runtime_of(g_app[(n)].rec)->defer)
#define PID_OF(n) (g_app[(n)].pid)
#define CB(n, off) mock_client_cb_get(&g_app[(n)].c, (off))

/* The mailbox as a client sees it, through the production dispatcher. */
static uint16_t svc_mailbox(uint16_t op, const uint8_t *req, uint16_t req_len,
			    uint8_t *reply, uint16_t *reply_len)
{
	return zzapp_mailbox_dispatch(op, req, req_len, reply, reply_len, 1,
				      ++svc_request_id, 0U);
}

static void svc_client_wire(mock_client *c)
{
	c->mailbox = svc_mailbox;
	c->kick = zzapp_xport_kick;
}

static void svc_select(unsigned n)
{
	zzapp_services_select_pid(g_app[n].pid);
}

/* A board token for a physical buffer address: what a client names in a
 * DISPLAY_OPEN completion. */
#define SVC_TOKEN(phys) ((uint32_t)(phys) - (uint32_t)MOCK_HEAP_TOKEN_ADJUST)

/* The record a mailbox CLAIM or a test admitted becomes app `n`: the client
 * is bound to its pair and served by mock_client_yield_hook. */
static void svc_bind(unsigned n, zzapp_rec_t *rec)
{
	zzapp_xport_buf_t pair;
	svc_app *a = &g_app[n];

	memset(a, 0, sizeof(*a));
	mock_client_init(&a->c);
	svc_client_wire(&a->c);
	a->rec = rec;
	a->handle = rec->hdr.handle;
	a->pid = rec->hdr.pid;
	if (zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_PAIR, &pair) != 1 ||
	    mock_client_attach(&a->c, pair.token) != AAR_OK) {
		fprintf(stderr, "svc_bind: app %u has no pair\n", n);
		exit(2);
	}
	(void)mock_client_register(&a->c);
}

/* App `n` the way the old implicit window record was: a record in the IDLE
 * state with its pair and an address space, selected for the services. */
static zzapp_rec_t __attribute__((unused)) *svc_app_new(unsigned n)
{
	zzapp_rec_t *rec = NULL;
	uint32_t handle = 0U;

	if (zzapp_rec_admit(&handle, &rec) != 0U ||
	    zzapp_xport_admit(rec) != AAR_REASON_NONE) {
		fprintf(stderr, "svc_app_new: admission of app %u refused\n", n);
		exit(2);
	}
	(void)zzapp_as_create(&rec->proc.as, rec->hdr.pid);
	(void)zzapp_rec_set_state(rec, ZZAPP_REC_LIVE);
	svc_bind(n, rec);
	svc_select(n);
	zzapp_services_set_state(AAR_STATE_IDLE);
	return rec;
}

/* App `n` from a CLAIM through the mailbox (CLAIMED, no address space). */
static zzapp_rec_t __attribute__((unused)) *svc_app_claim(unsigned n)
{
	zzapp_rec_t *rec;
	mock_client probe;

	mock_client_init(&probe);
	svc_client_wire(&probe);
	if (mock_client_claim(&probe) != SDK_STATUS_OK)
		return NULL;
	rec = zzapp_rec_by_handle(probe.handle);
	if (!rec)
		return NULL;
	svc_bind(n, rec);
	svc_select(n);
	return rec;
}

/* A claimed record has no address space until the loader builds one at
 * LOAD_END; tests that grant buffers to it make one. */
static void __attribute__((unused)) give_space(unsigned n)
{
	zzapp_rec_t *rec = g_app[n].rec;

	(void)zzapp_as_create(&rec->proc.as, rec->hdr.pid);
}

static void __attribute__((unused)) test_force_state(unsigned n, uint32_t state)
{
	g_app[n].rec->claim.state = state;
	zzapp_xport_cb_set_state(g_app[n].rec, state, 0U);
}

static void svc_loader_begin_ok(void)
{
}

/* LOAD_BEGIN of app `n` with the client capabilities `caps`, through the
 * production path: dispatched from core 0, run by the system thread on core 1
 * (the stub loader's begin succeeds), harvested and acknowledged; the app is
 * selected afterwards. Returns the completion's status and, when asked, its
 * reply payload. */
static uint16_t __attribute__((unused)) svc_load_begin(unsigned n, uint32_t caps,
							uint8_t *reply, uint16_t *reply_len)
{
	uint8_t scratch[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint16_t status = 0U, len = 0U;
	void (*saved)(void) = service_test_loader_begin_hook;

	service_test_loader_begin_hook = svc_loader_begin_ok;
	zzapp_services_test_set_core(0);
	if (mock_client_load_begin(&g_app[n].c, caps, NULL, NULL) != SDK_STATUS_QUEUED) {
		fprintf(stderr, "svc_load_begin: not queued\n");
		exit(2);
	}
	zzapp_services_test_set_core(1);
	zzapp_services_select_pid(g_app[n].pid);
	zzapp_services_poll();
	service_test_loader_begin_hook = saved;
	if (!zzapp_mailbox_harvest(NULL, NULL, NULL, &status, reply ? reply : scratch,
				   &len)) {
		fprintf(stderr, "svc_load_begin: no completion\n");
		exit(2);
	}
	zzapp_mailbox_harvest_ack();
	if (reply_len)
		*reply_len = len;
	zzapp_services_select_pid(g_app[n].pid);
	return status;
}

/* The state a client reads from its control block. */
#define CBSTATE(n) CB((n), AAR_CB_STATE)

/* The alias a surface or an audio ring reaches the app by is a virtual address
 * of its space; the buffer it names is checked by the physical address. */
#define PHYS(alias) \
	zzapp_as_alias_phys(&service_proc()->as, (uint32_t)(uintptr_t)(alias))

/* The yield of a parked app: the Amiga serves every client, then core 1's
 * pass wakes what is ready. */
static void __attribute__((unused)) svc_yield(void)
{
	mock_client_yield_hook();
	zzapp_services_poll();
}

/* Between sub-tests: every app's record is released the way a claim ends
 * (audio detached, tables back, blocks returned, handle freed), the heap and
 * the client registry start empty and the services forget their statics. */
static void __attribute__((unused)) svc_apps_reset(void)
{
	zzapp_xport_env_t env;
	unsigned n;

	mock_client_registry_clear();
	for (n = 0U; n < SVC_MAX_APPS; n++) {
		svc_app *a = &g_app[n];
		zzapp_rec_t *rec = a->handle ? zzapp_rec_by_handle(a->handle) : NULL;

		if (rec) {
			zzapp_services_select_pid(a->pid);
			audio_detach();
			zzapp_rec_retire_tables(rec);
			(void)zzapp_xport_release(rec);
			(void)zzapp_rec_free(a->handle);
		}
		memset(a, 0, sizeof(*a));
	}
	zzapp_services_test_reset();
	mock_heap_reset();
	env = mock_heap_env();
	zzapp_xport_init(&env);
	svc_request_id = 0U;
}

#endif
