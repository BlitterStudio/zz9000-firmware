/*
 * Executable lock-order checks for zz9k.library resident synchronization and
 * the dead-owner check of its openers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "zz9k/library.h"
#include <stdio.h>
#include <string.h>
#include "zz9k_library_task_stamp.h"

enum TestOwner {
  TEST_OWNER_NONE,
  TEST_OWNER_DISPATCHER,
  TEST_OWNER_OTHER
};

struct TestSemaphore {
  enum TestOwner owner;
  unsigned int obtains;
  unsigned int attempts;
  unsigned int releases;
};

/* A world for the dead-owner check: the tasks Exec still has, the openers
 * the library holds, and what the release step did. */
#define REAP_TASKS 4U

struct ReapOpener {
  struct ReapOpener *next;
  ZZ9KTaskStamp stamp;
  unsigned char dead;
  unsigned char busy_once;
  unsigned int release_attempts;
  unsigned int released;
  unsigned int released_at;
  unsigned char closing;
};

struct ReapWorld {
  struct ReapOpener *openers;
  ZZ9KTaskStamp tasks[REAP_TASKS];
  unsigned char present[REAP_TASKS];
  uint32_t clock;
  uint32_t last_check;
  unsigned char scan_pending;
  unsigned int sequence;
  unsigned int stops;
  unsigned int stopped_at;
  unsigned int backoffs;
};

static inline void zz9k_lib_reap_service(void *base);
static inline void zz9k_lib_reap_release(void *base);

struct TestCall {
  unsigned char submitted;
  unsigned char completed;
  unsigned char cancelled;
  unsigned char signaled;
  unsigned char tick_armed;
  unsigned int writes;
  int status;
};

struct TestState {
  struct TestSemaphore transport_lock;
  struct TestCall first;
  struct TestCall second;
  struct TestCall foreign;
  int complete_on_second_poll;
  int run_second_while_first_waits;
  int second_status;
  int first_waits;
  int second_submit_before_first_wait;
  int break_signal;
  int set_break_while_waiting;
  int complete_after_tick;
  int complete_on_next_poll;
  int complete_on_deadline;
  int completion_pending;
  struct ReapWorld reap;
  uint32_t last_wait_mask;
};

static struct TestState *test_state;
static int failures;

static void mock_obtain(void *opaque)
{
  struct TestSemaphore *semaphore = (struct TestSemaphore *)opaque;

  if (semaphore->owner != TEST_OWNER_NONE) {
    printf("synchronous waiter acquired an already-held transport lock\n");
    failures++;
    return;
  }
  semaphore->owner = TEST_OWNER_DISPATCHER;
  semaphore->obtains++;
}

static int mock_attempt(void *opaque)
{
  struct TestSemaphore *semaphore = (struct TestSemaphore *)opaque;

  semaphore->attempts++;
  if (semaphore->owner != TEST_OWNER_NONE) {
    return 0;
  }
  semaphore->owner = TEST_OWNER_DISPATCHER;
  return 1;
}

static void mock_release(void *opaque)
{
  struct TestSemaphore *semaphore = (struct TestSemaphore *)opaque;

  if (semaphore->owner != TEST_OWNER_DISPATCHER) {
    failures++;
    return;
  }
  semaphore->owner = TEST_OWNER_NONE;
  semaphore->releases++;
}

static inline void zz9k_lib_sync_call_signal(void *task,
                                              uint32_t signal_mask);

static void mock_signal(void *opaque, uint32_t signal_mask)
{
  struct TestCall *call = (struct TestCall *)opaque;

  if (signal_mask == 0U) {
    failures++;
    return;
  }
  call->signaled = 1U;
}

static void mock_complete(struct TestCall *call)
{
  if (call->submitted && !call->cancelled) {
    call->completed = 1U;
    zz9k_lib_sync_call_signal(call, 1U);
    call->writes++;
    call->status = ZZ9K_STATUS_OK;
  }
}

static int mock_call_submit(void *base, void *opaque)
{
  struct TestCall *call = (struct TestCall *)opaque;

  (void)base;
  if (test_state->transport_lock.owner != TEST_OWNER_DISPATCHER) {
    failures++;
  }
  if (call == &test_state->second && test_state->first_waits == 0) {
    test_state->second_submit_before_first_wait++;
  }
  call->submitted = 1U;
  call->status = ZZ9K_STATUS_QUEUED;
  return ZZ9K_STATUS_QUEUED;
}

static int mock_call_poll(void *base, void *opaque)
{
  struct TestCall *call = (struct TestCall *)opaque;

  (void)base;
  if (test_state->transport_lock.owner != TEST_OWNER_DISPATCHER) {
    failures++;
  }
  if (test_state->complete_on_second_poll &&
      call == &test_state->second) {
    mock_complete(&test_state->first);
    mock_complete(&test_state->foreign);
    mock_complete(&test_state->second);
    test_state->complete_on_second_poll = 0;
  }
  if (test_state->complete_on_next_poll && call == &test_state->first) {
    test_state->complete_on_next_poll = 0;
    mock_complete(call);
  }
  if (test_state->completion_pending && call == &test_state->first) {
    test_state->completion_pending = 0;
    mock_complete(call);
  }
  return ZZ9K_STATUS_OK;
}

static int mock_call_completed(void *opaque)
{
  return ((struct TestCall *)opaque)->completed != 0U;
}

static int mock_call_status(void *opaque)
{
  return ((struct TestCall *)opaque)->status;
}

static int mock_call_cancel(void *base, void *opaque)
{
  struct TestCall *call = (struct TestCall *)opaque;

  (void)base;
  if (test_state->transport_lock.owner != TEST_OWNER_DISPATCHER) {
    failures++;
  }
  call->cancelled = 1U;
  return ZZ9K_STATUS_OK;
}

static inline int zz9k_lib_sync_call(void *base, void *transport_lock,
                                     void *record);
static int mock_call_wait(void *opaque);

static uint32_t mock_wait_block(void *opaque, uint32_t signal_mask)
{
  struct TestCall *call = (struct TestCall *)opaque;
  int status;

  test_state->last_wait_mask = signal_mask;
  status = mock_call_wait(opaque);
  if (status == ZZ9K_STATUS_TIMEOUT) {
    return 2U;
  }
  return call->signaled ? 1U : 4U;
}

static int mock_call_wait(void *opaque)
{
  struct TestCall *call = (struct TestCall *)opaque;

  if (test_state->set_break_while_waiting) {
    test_state->break_signal = 1;
  }

  if (call == &test_state->first) {
    if (test_state->complete_after_tick) {
      test_state->complete_after_tick = 0;
      test_state->complete_on_next_poll = 1;
      return ZZ9K_STATUS_OK;
    }
    if (test_state->complete_on_deadline) {
      test_state->complete_on_deadline = 0;
      test_state->completion_pending = 1;
      return ZZ9K_STATUS_TIMEOUT;
    }
    test_state->first_waits++;
    if (test_state->run_second_while_first_waits) {
      test_state->run_second_while_first_waits = 0;
      test_state->second_status = zz9k_lib_sync_call(
          test_state, &test_state->transport_lock, &test_state->second);
      return ZZ9K_STATUS_OK;
    }
  }
  return ZZ9K_STATUS_TIMEOUT;
}

static struct ReapWorld *reap_of(void *base)
{
  return &((struct TestState *)base)->reap;
}

static int mock_reap_lookup(void *base, const void *task, ZZ9KTaskStamp *out)
{
  struct ReapWorld *world = reap_of(base);
  unsigned int i;

  for (i = 0U; i < REAP_TASKS; i++) {
    if (world->present[i] && world->tasks[i].task == task) {
      *out = world->tasks[i];
      return 1;
    }
  }
  return 0;
}

static int mock_reap_release_opener(void *base, struct ReapOpener *opener)
{
  struct ReapWorld *world = reap_of(base);
  struct ReapOpener **link;

  if (test_state->transport_lock.owner != TEST_OWNER_DISPATCHER) {
    printf("an opener was released without the transport lock\n");
    failures++;
  }
  opener->release_attempts++;
  if (opener->busy_once) {
    opener->busy_once--;
    return 0;
  }
  opener->released++;
  opener->released_at = ++world->sequence;
  for (link = &world->openers; *link; link = &(*link)->next) {
    if (*link == opener) {
      *link = opener->next;
      break;
    }
  }
  return 1;
}

static void mock_reap_stop_dispatcher(void *base)
{
  struct ReapWorld *world = reap_of(base);

  if (test_state->transport_lock.owner != TEST_OWNER_NONE) {
    printf("the dispatcher was stopped with the transport lock held\n");
    failures++;
  }
  world->stops++;
  world->stopped_at = ++world->sequence;
}

static void mock_reap_backoff(void *base)
{
  reap_of(base)->backoffs++;
  mock_release(&test_state->transport_lock);
  mock_obtain(&test_state->transport_lock);
}

/* Exec's list nodes: the successor first, and a tail node whose successor is
 * null. */
struct FakeNode {
  struct FakeNode *succ;
};

#define ZZ9K_LIB_REAP_TASK_LOOKUP(base, task, out) \
  mock_reap_lookup(base, task, out)
#define ZZ9K_LIB_REAP_CLOCK(base) (reap_of(base)->clock)
#define ZZ9K_LIB_REAP_LAST_CHECK(base) (reap_of(base)->last_check)
#define ZZ9K_LIB_REAP_SCAN_PENDING(base) (reap_of(base)->scan_pending)
#define ZZ9K_LIB_REAP_OPENERS_FIRST(base) (reap_of(base)->openers)
#define ZZ9K_LIB_REAP_OPENER_NEXT(opener) \
  (((struct ReapOpener *)(opener))->next)
#define ZZ9K_LIB_REAP_OPENER_DEAD(opener) \
  (((struct ReapOpener *)(opener))->dead)
#define ZZ9K_LIB_REAP_OPENER_STAMP(opener) \
  (&((struct ReapOpener *)(opener))->stamp)
#define ZZ9K_LIB_REAP_OPENER_CLOSING(opener) \
  (((struct ReapOpener *)(opener))->closing)
#define ZZ9K_LIB_REAP_NODE_SUCC(node) (((const struct FakeNode *)(node))->succ)
#define ZZ9K_LIB_REAP_RELEASE_OPENER(base, opener) \
  mock_reap_release_opener(base, (struct ReapOpener *)(opener))
#define ZZ9K_LIB_REAP_LOCK(base) mock_obtain(&test_state->transport_lock)
#define ZZ9K_LIB_REAP_UNLOCK(base) mock_release(&test_state->transport_lock)
#define ZZ9K_LIB_REAP_BACKOFF(base) mock_reap_backoff(base)
#define ZZ9K_LIB_REAP_STOP_DISPATCHER(base) mock_reap_stop_dispatcher(base)
#include "zz9k_library_owner_reap.h"

#define ZZ9K_LIB_SYNC_OBTAIN(lock) mock_obtain(lock)
#define ZZ9K_LIB_SYNC_ATTEMPT(lock) mock_attempt(lock)
#define ZZ9K_LIB_SYNC_RELEASE(lock) mock_release(lock)
#define ZZ9K_LIB_SYNC_REAP_SERVICE(base) zz9k_lib_reap_service((void *)(base))
#define ZZ9K_LIB_SYNC_REAP_RELEASE(base) zz9k_lib_reap_release((void *)(base))
#define ZZ9K_LIB_SYNC_CALL_SUBMIT(base, record) \
  mock_call_submit(base, record)
#define ZZ9K_LIB_SYNC_CALL_POLL(base, record) mock_call_poll(base, record)
#define ZZ9K_LIB_SYNC_CALL_COMPLETED(record) mock_call_completed(record)
#define ZZ9K_LIB_SYNC_CALL_DRAINED(record) 0
#define ZZ9K_LIB_SYNC_CALL_STATUS(record) mock_call_status(record)
#define ZZ9K_LIB_SYNC_CALL_CANCEL(base, record) \
  mock_call_cancel(base, record)
#define ZZ9K_LIB_SYNC_CALL_WAIT(record) zz9k_lib_sync_call_wait_step(record)
#define ZZ9K_LIB_SYNC_CALL_SIGNAL(task, signal_mask) \
  mock_signal(task, signal_mask)
#define ZZ9K_LIB_SYNC_WAIT_BUSY_AVAILABLE(record) 0
#define ZZ9K_LIB_SYNC_WAIT_BUSY_DONE(record) 1
#define ZZ9K_LIB_SYNC_WAIT_MARK_BUSY_DONE(record) do { } while (0)
#define ZZ9K_LIB_SYNC_WAIT_START_BUSY(record) do { } while (0)
#define ZZ9K_LIB_SYNC_WAIT_BUSY_EXPIRED(record) 1
#define ZZ9K_LIB_SYNC_WAIT_FINISH_BUSY(record) do { } while (0)
#define ZZ9K_LIB_SYNC_WAIT_TIMEOUT_ZERO(record) 0
#define ZZ9K_LIB_SYNC_WAIT_TIMERS_AVAILABLE(record) 1
#define ZZ9K_LIB_SYNC_WAIT_TICK_ARMED(record) \
  ((struct TestCall *)(record))->tick_armed
#define ZZ9K_LIB_SYNC_WAIT_ARM_TICK(record) \
  (((struct TestCall *)(record))->tick_armed = 1U)
#define ZZ9K_LIB_SYNC_WAIT_OWNER_MASK(record) 1U
#define ZZ9K_LIB_SYNC_WAIT_DEADLINE_MASK(record) 2U
#define ZZ9K_LIB_SYNC_WAIT_TICK_MASK(record) 4U
#define ZZ9K_LIB_SYNC_WAIT_BREAK_MASK(record) 8U
#define ZZ9K_LIB_SYNC_WAIT_BLOCK(record, mask) mock_wait_block(record, mask)
#define ZZ9K_LIB_SYNC_WAIT_FINISH_TICK(record) \
  (((struct TestCall *)(record))->tick_armed = 0U)
#define ZZ9K_LIB_SYNC_WAIT_FALLBACK(record) ZZ9K_STATUS_TIMEOUT
#include "zz9k_library_resident_sync.h"
#undef ZZ9K_LIB_SYNC_OBTAIN
#undef ZZ9K_LIB_SYNC_ATTEMPT
#undef ZZ9K_LIB_SYNC_RELEASE
#undef ZZ9K_LIB_SYNC_REAP_SERVICE
#undef ZZ9K_LIB_SYNC_REAP_RELEASE
#undef ZZ9K_LIB_SYNC_CALL_SUBMIT
#undef ZZ9K_LIB_SYNC_CALL_POLL
#undef ZZ9K_LIB_SYNC_CALL_COMPLETED
#undef ZZ9K_LIB_SYNC_CALL_STATUS
#undef ZZ9K_LIB_SYNC_CALL_DRAINED
#undef ZZ9K_LIB_SYNC_CALL_CANCEL
#undef ZZ9K_LIB_SYNC_CALL_WAIT
#undef ZZ9K_LIB_SYNC_CALL_SIGNAL
#undef ZZ9K_LIB_SYNC_WAIT_BUSY_AVAILABLE
#undef ZZ9K_LIB_SYNC_WAIT_BUSY_DONE
#undef ZZ9K_LIB_SYNC_WAIT_MARK_BUSY_DONE
#undef ZZ9K_LIB_SYNC_WAIT_START_BUSY
#undef ZZ9K_LIB_SYNC_WAIT_BUSY_EXPIRED
#undef ZZ9K_LIB_SYNC_WAIT_FINISH_BUSY
#undef ZZ9K_LIB_SYNC_WAIT_TIMEOUT_ZERO
#undef ZZ9K_LIB_SYNC_WAIT_TIMERS_AVAILABLE
#undef ZZ9K_LIB_SYNC_WAIT_TICK_ARMED
#undef ZZ9K_LIB_SYNC_WAIT_ARM_TICK
#undef ZZ9K_LIB_SYNC_WAIT_OWNER_MASK
#undef ZZ9K_LIB_SYNC_WAIT_DEADLINE_MASK
#undef ZZ9K_LIB_SYNC_WAIT_TICK_MASK
#undef ZZ9K_LIB_SYNC_WAIT_BREAK_MASK
#undef ZZ9K_LIB_SYNC_WAIT_BLOCK
#undef ZZ9K_LIB_SYNC_WAIT_FINISH_TICK
#undef ZZ9K_LIB_SYNC_WAIT_FALLBACK

static void expect(int condition, const char *message)
{
  if (!condition) {
    printf("%s\n", message);
    failures++;
  }
}

static void init_state(struct TestState *state)
{
  memset(state, 0, sizeof(*state));
  test_state = state;
}

static void test_concurrent_waiters_route_and_wake_owners(void)
{
  struct TestState state;

  init_state(&state);
  state.foreign.submitted = 1U;
  state.complete_on_second_poll = 1;
  state.run_second_while_first_waits = 1;
  expect(zz9k_lib_sync_call(&state, &state.transport_lock, &state.first) ==
             ZZ9K_STATUS_OK,
         "first synchronous caller did not receive its completion");
  expect(state.second_status == ZZ9K_STATUS_OK,
         "second synchronous caller did not receive its completion");
  expect(state.first.signaled && state.second.signaled,
         "drainer did not wake both synchronous owners");
  expect(state.foreign.signaled && state.foreign.writes == 1U,
         "foreign completion was not routed while callers waited");
  expect(state.first_waits == 1 && state.second_submit_before_first_wait == 0,
         "slow caller held the transport lock across the second submit");
  expect(state.transport_lock.owner == TEST_OWNER_NONE,
         "synchronous callers retained the transport lock");
}

static void test_late_completion_self_drains_on_tick(void)
{
  struct TestState state;

  init_state(&state);
  state.complete_after_tick = 1;
  expect(zz9k_lib_sync_call(&state, &state.transport_lock, &state.first) ==
             ZZ9K_STATUS_OK,
         "sync caller did not self-drain a late completion");
  expect(state.first.completed && state.first.signaled,
         "self-drained completion did not wake its owner");
  expect((state.last_wait_mask & 4U) != 0U,
         "sync wait omitted the periodic drain tick");
}

static void test_deadline_drains_late_completion(void)
{
  struct TestState state;

  init_state(&state);
  state.complete_on_deadline = 1;
  expect(zz9k_lib_sync_call(&state, &state.transport_lock, &state.first) ==
             ZZ9K_STATUS_OK,
         "deadline wake did not drain an already-arrived completion");
  expect(state.first.completed && !state.first.cancelled,
         "deadline wake cancelled an already-arrived completion");
}

static void test_timeout_abandons_pending_record(void)
{
  struct TestState state;

  init_state(&state);
  expect(zz9k_lib_sync_call(&state, &state.transport_lock, &state.first) ==
             ZZ9K_STATUS_TIMEOUT,
         "timed-out synchronous caller did not report timeout");
  expect(state.first.cancelled,
         "timed-out synchronous caller retained a pending record");
  mock_complete(&state.first);
  expect(state.first.writes == 0U && !state.first.completed,
         "late completion wrote into an abandoned synchronous record");
}

static void test_break_signal_remains_pending(void)
{
  struct TestState state;

  init_state(&state);
  state.break_signal = 1;
  expect(zz9k_lib_sync_call(&state, &state.transport_lock, &state.first) ==
             ZZ9K_STATUS_TIMEOUT,
         "pre-existing break signal changed synchronous call status");
  expect(state.break_signal,
         "synchronous call consumed a pre-existing break signal");
  expect((state.last_wait_mask & 8U) == 0U,
         "synchronous call waited on a caller break signal");

  init_state(&state);
  state.set_break_while_waiting = 1;
  expect(zz9k_lib_sync_call(&state, &state.transport_lock, &state.first) ==
             ZZ9K_STATUS_TIMEOUT,
         "new break signal changed synchronous call status");
  expect(state.break_signal,
         "synchronous call consumed a break signal raised while waiting");
  expect((state.last_wait_mask & 8U) == 0U,
         "synchronous call waited on a newly raised break signal");
}

/* One dispatcher pass at one more vertical blank, as the service task runs
 * it: note whether a check is due, then the release step if it got the
 * transport lock. */
static void reap_pass(struct TestState *state)
{
  state->reap.clock++;
  if (zz9k_lib_dispatcher_service(state, &state->transport_lock)) {
    mock_release(&state->transport_lock);
  }
}

static void reap_add_task(struct ReapWorld *world, unsigned int index,
                          unsigned int id)
{
  ZZ9KTaskStamp *stamp = &world->tasks[index];

  memset(stamp, 0, sizeof(*stamp));
  stamp->task = (const void *)(uintptr_t)(0x1000U + id * 0x100U);
  stamp->name = (const void *)(uintptr_t)(0x9000U + id);
  stamp->type = 13U;
  stamp->task_num = id;
  world->present[index] = 1U;
}

static void reap_add_opener(struct ReapWorld *world,
                            struct ReapOpener *opener,
                            unsigned int task_index)
{
  memset(opener, 0, sizeof(*opener));
  opener->stamp = world->tasks[task_index];
  opener->next = world->openers;
  world->openers = opener;
}

static void test_dispatcher_defers_to_a_busy_transport(void)
{
  struct TestState state;
  unsigned int pass;

  init_state(&state);
  reap_add_task(&state.reap, 0U, 1U);
  state.transport_lock.owner = TEST_OWNER_OTHER;
  for (pass = 0U; pass < ZZ9K_LIB_REAP_PERIOD_TICKS; pass++) {
    state.reap.clock++;
    expect(!zz9k_lib_dispatcher_service(&state, &state.transport_lock),
           "dispatcher acquired transport held by another task");
  }
  expect(state.transport_lock.obtains == 0U,
         "dispatcher blocked obtaining a busy transport");
  expect(state.transport_lock.attempts == ZZ9K_LIB_REAP_PERIOD_TICKS,
         "dispatcher did not attempt the busy transport once per pass");
  /* The check came due and waits for the lock; nothing was released. */
  expect(state.reap.scan_pending == 1U,
         "a due check was lost while the transport was busy");
  state.transport_lock.owner = TEST_OWNER_NONE;
  reap_pass(&state);
  expect(state.reap.scan_pending == 0U &&
             state.transport_lock.owner == TEST_OWNER_NONE,
         "the pending check did not run once the transport was free");
}

static void test_reap_leaves_a_live_owner_alone(void)
{
  struct TestState state;
  struct ReapOpener opener;
  unsigned int pass;

  init_state(&state);
  reap_add_task(&state.reap, 0U, 1U);
  reap_add_opener(&state.reap, &opener, 0U);
  for (pass = 0U; pass < 4U * ZZ9K_LIB_REAP_PERIOD_TICKS; pass++) {
    reap_pass(&state);
  }
  expect(!opener.dead && opener.release_attempts == 0U,
         "a live owner's opener was released");
  expect(state.reap.openers == &opener, "a live owner's opener left the list");
}

static void test_reap_releases_a_gone_owner_within_the_period(void)
{
  struct TestState state;
  struct ReapOpener opener;
  unsigned int passes;
  unsigned int pass;

  init_state(&state);
  reap_add_task(&state.reap, 0U, 1U);
  reap_add_opener(&state.reap, &opener, 0U);
  for (pass = 0U; pass < 10U; pass++) {
    reap_pass(&state);
  }
  state.reap.present[0] = 0U;
  for (passes = 0U; passes < 2U * ZZ9K_LIB_REAP_PERIOD_TICKS &&
                    opener.released == 0U; passes++) {
    reap_pass(&state);
  }
  expect(opener.released == 1U, "a gone owner's opener was never released");
  expect(passes <= ZZ9K_LIB_REAP_PERIOD_TICKS,
         "a gone owner was released later than the check period");
  expect(state.reap.openers == 0, "the released opener stayed on the list");
  expect(state.transport_lock.owner == TEST_OWNER_NONE,
         "the release left a lock held");
  /* Nothing more happens, and the record is not touched again. */
  for (pass = 0U; pass < 3U * ZZ9K_LIB_REAP_PERIOD_TICKS; pass++) {
    reap_pass(&state);
  }
  expect(opener.released == 1U, "a released opener was released twice");
}

static void test_reap_treats_address_reuse_as_gone(void)
{
  struct TestState state;
  struct ReapOpener opener;
  struct ReapWorld *world = &state.reap;
  unsigned int variant;
  unsigned int pass;

  for (variant = 0U; variant < 4U; variant++) {
    init_state(&state);
    reap_add_task(world, 0U, 1U);
    reap_add_opener(world, &opener, 0U);
    /* The old task is gone; a new one sits at the same address. */
    switch (variant) {
    case 0: /* a different name pointer */
      world->tasks[0].name = (const void *)(uintptr_t)0xA000U;
      break;
    case 1: /* a different CLI task number */
      world->tasks[0].task_num = 7U;
      break;
    case 2: /* a different node type */
      world->tasks[0].type = 1U;
      break;
    default: /* a task that matches every recorded field is taken for the
              * owner: the client id stays held, as it does without the
              * check */
      break;
    }
    for (pass = 0U; pass < 2U * ZZ9K_LIB_REAP_PERIOD_TICKS; pass++) {
      reap_pass(&state);
    }
    if (variant < 3U) {
      expect(opener.released == 1U,
             "a task reusing the owner's address kept the owner's opener");
    } else {
      expect(opener.released == 0U && !opener.dead,
             "a task matching the owner in every field lost its opener");
    }
  }
}

static void test_reap_releases_dead_openers_and_retries_busy(void)
{
  struct TestState state;
  struct ReapOpener gone;
  struct ReapOpener live;
  unsigned int pass;

  init_state(&state);
  reap_add_task(&state.reap, 0U, 1U);
  reap_add_task(&state.reap, 1U, 2U);
  reap_add_opener(&state.reap, &gone, 0U);
  reap_add_opener(&state.reap, &live, 1U);
  gone.busy_once = 1U;
  state.reap.present[0] = 0U;
  for (pass = 0U; pass < ZZ9K_LIB_REAP_PERIOD_TICKS + 2U; pass++) {
    reap_pass(&state);
  }
  expect(gone.released == 1U && gone.release_attempts == 2U,
         "a gone task's opener was not released after the firmware's BUSY");
  expect(live.release_attempts == 0U && !live.dead,
         "a live task's opener was released");
  expect(state.reap.openers == &live, "the live opener left the list");
}

static void test_reap_releases_a_dead_orphaned_opener(void)
{
  struct TestState state;
  struct ReapOpener orphan;

  /* An opener orphaned when its address was reused is dead from the start,
   * and goes at the first pass that holds the transport lock. */
  init_state(&state);
  reap_add_task(&state.reap, 0U, 1U);
  reap_add_opener(&state.reap, &orphan, 0U);
  orphan.dead = 1U;
  reap_pass(&state);
  expect(orphan.released == 1U, "an orphaned opener was not released");
}

/* CloseLibrary and the service task must not both free an opener. */
static void test_reap_leaves_an_opener_in_closelibrary_alone(void)
{
  struct TestState state;
  struct ReapOpener closing;
  unsigned int pass;

  init_state(&state);
  reap_add_task(&state.reap, 0U, 1U);
  reap_add_opener(&state.reap, &closing, 0U);
  closing.closing = 1U;
  state.reap.present[0] = 0U;
  for (pass = 0U; pass < 3U * ZZ9K_LIB_REAP_PERIOD_TICKS; pass++) {
    reap_pass(&state);
  }
  expect(!closing.dead && closing.release_attempts == 0U &&
             closing.released == 0U,
         "an opener in CloseLibrary was released by the service task");
  /* Dead before CloseLibrary began is a different state: the service task
   * owns it, and CloseLibrary leaves it. A closing flag set on a record
   * that was already dead is still not released. */
  closing.dead = 1U;
  reap_pass(&state);
  expect(closing.released == 0U,
         "a closing opener that was also marked dead was released");
}

/* The task lookup over a fake Exec: the running task, and nodes on the ready
 * and waiting lists, each ending at a tail node. */
static void test_task_lookup_walks_the_exec_lists(void)
{
  struct FakeNode ready_tail = { 0 };
  struct FakeNode wait_tail = { 0 };
  struct FakeNode ready_b = { &ready_tail };
  struct FakeNode ready_a = { &ready_b };
  struct FakeNode wait_c = { &wait_tail };
  struct FakeNode running = { 0 };
  struct FakeNode gone = { 0 };

  expect(zz9k_lib_reap_task_listed(&running, &running, &ready_a, &wait_c),
         "the running task was not found");
  expect(zz9k_lib_reap_task_listed(&ready_a, &running, &ready_a, &wait_c) &&
             zz9k_lib_reap_task_listed(&ready_b, &running, &ready_a, &wait_c),
         "a task on the ready list was not found");
  expect(zz9k_lib_reap_task_listed(&wait_c, &running, &ready_a, &wait_c),
         "a task on the waiting list was not found");
  expect(!zz9k_lib_reap_task_listed(&gone, &running, &ready_a, &wait_c),
         "a task on no list was found");
  expect(!zz9k_lib_reap_task_listed(&ready_tail, &running, &ready_a, &wait_c) &&
             !zz9k_lib_reap_task_listed(&wait_tail, &running, &ready_a,
                                        &wait_c),
         "a list's tail node was taken for a task");
  /* Empty lists: the head is the tail node. */
  expect(!zz9k_lib_reap_task_listed(&gone, &running, &ready_tail, &wait_tail),
         "a task was found on empty lists");
  expect(zz9k_lib_reap_task_listed(&running, &running, &ready_tail,
                                   &wait_tail),
         "the running task was not found with empty lists");
}

/* Who owns an opener when Open, CloseLibrary and the service task meet. */
static void test_opener_ownership_rules(void)
{
  ZZ9KTaskStamp recorded;
  ZZ9KTaskStamp now;

  memset(&recorded, 0, sizeof(recorded));
  recorded.task = (const void *)(uintptr_t)0x1000U;
  recorded.name = (const void *)(uintptr_t)0x2000U;
  recorded.task_num = 3U;
  recorded.type = 13U;
  now = recorded;

  /* Open: a live record of the same task is shared; a dead one, or one made
   * by another task at this address, is not. */
  expect(!zz9k_lib_opener_stale(0, &recorded, &now),
         "Open treated its own live record as stale");
  expect(zz9k_lib_opener_stale(1, &recorded, &now),
         "Open shared a dead record");
  now.name = (const void *)(uintptr_t)0x3000U;
  expect(zz9k_lib_opener_stale(0, &recorded, &now),
         "Open shared a record made by another task at this address");

  /* CloseLibrary: only the last reference releases, and a record the
   * service task already took is left to it. */
  expect(zz9k_lib_opener_close_action(0, 2U) == ZZ9K_OPENER_CLOSE_UNREFERENCE,
         "a close with references left released the opener");
  expect(zz9k_lib_opener_close_action(0, 1U) == ZZ9K_OPENER_CLOSE_RELEASE,
         "the last close did not release the opener");
  expect(zz9k_lib_opener_close_action(1, 1U) ==
             ZZ9K_OPENER_CLOSE_LEAVE_TO_REAPER,
         "the last close freed an opener the service task owns");
  expect(zz9k_lib_opener_close_action(1, 2U) == ZZ9K_OPENER_CLOSE_UNREFERENCE,
         "a close with references left touched a dead opener");

  /* The service task: never a record CloseLibrary is releasing. */
  expect(zz9k_lib_opener_markable(0, 0) && !zz9k_lib_opener_markable(1, 0) &&
             !zz9k_lib_opener_markable(0, 1),
         "the service task marked a dead or closing opener");
  expect(zz9k_lib_opener_reapable(1, 0) && !zz9k_lib_opener_reapable(0, 0) &&
             !zz9k_lib_opener_reapable(1, 1),
         "the service task released a live or closing opener");
}

/* The last CloseLibrary finishes every opener release the service task left
 * pending (BUSY) before it stops the dispatcher, and frees each record once. */
static void test_final_close_finishes_pending_opener_releases(void)
{
  struct TestState state;
  struct ReapOpener pending;
  struct ReapOpener accepted;
  struct ReapOpener live;

  init_state(&state);
  reap_add_task(&state.reap, 0U, 1U);
  reap_add_task(&state.reap, 1U, 2U);
  reap_add_task(&state.reap, 2U, 3U);
  reap_add_opener(&state.reap, &pending, 0U);
  reap_add_opener(&state.reap, &accepted, 1U);
  reap_add_opener(&state.reap, &live, 2U);
  /* A renamed live task's opener: marked dead, its release answered BUSY
   * twice by the firmware, and its final CloseLibrary left it to the
   * service task. */
  pending.dead = 1U;
  pending.busy_once = 2U;
  accepted.dead = 1U;
  live.closing = 1U;

  zz9k_lib_reap_final_close(&state);

  expect(pending.released == 1U && pending.release_attempts == 3U,
         "a pending release was not retried until the firmware took it");
  expect(accepted.released == 1U && accepted.release_attempts == 1U,
         "a dead opener was not released exactly once");
  expect(live.released == 0U && live.release_attempts == 0U,
         "an opener another close is releasing was taken over");
  expect(state.reap.stops == 1U && state.reap.stopped_at >
             pending.released_at && state.reap.stopped_at >
             accepted.released_at,
         "the dispatcher stopped before every opener release finished");
  expect(state.reap.backoffs == 2U,
         "the retries did not back off with the lock dropped");
  expect(state.reap.openers == &live && live.next == 0,
         "a released opener stayed on the list");
  expect(state.transport_lock.owner == TEST_OWNER_NONE,
         "the final close left the lock held");
  /* The service task no longer sees the taken-over records. */
  reap_pass(&state);
  expect(pending.release_attempts == 3U && accepted.release_attempts == 1U,
         "the service task released an opener twice");
}

static void test_reap_clock_survives_wraparound(void)
{
  uint32_t last = 0xFFFFFFF0U;

  expect(!zz9k_lib_reap_due(&last, 0xFFFFFFF0U + 49U, 50U),
         "the check came due early");
  expect(zz9k_lib_reap_due(&last, 0xFFFFFFF0U + 50U, 50U) && last == 34U,
         "the check did not come due across a counter wrap");
}

int main(void)
{
  test_concurrent_waiters_route_and_wake_owners();
  test_timeout_abandons_pending_record();
  test_break_signal_remains_pending();
  test_late_completion_self_drains_on_tick();
  test_deadline_drains_late_completion();
  test_dispatcher_defers_to_a_busy_transport();
  test_reap_leaves_a_live_owner_alone();
  test_task_lookup_walks_the_exec_lists();
  test_opener_ownership_rules();
  test_final_close_finishes_pending_opener_releases();
  test_reap_releases_a_gone_owner_within_the_period();
  test_reap_treats_address_reuse_as_gone();
  test_reap_releases_dead_openers_and_retries_busy();
  test_reap_releases_a_dead_orphaned_opener();
  test_reap_leaves_an_opener_in_closelibrary_alone();
  test_reap_clock_survives_wraparound();
  return failures ? 1 : 0;
}
