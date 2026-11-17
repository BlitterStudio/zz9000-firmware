/*
 * Executable lock-order checks for zz9k.library resident synchronization.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "zz9k/library.h"
#include <stdio.h>
#include <string.h>

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

struct TestHandle {
  unsigned char event_posted;
  unsigned int heartbeat;
};

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
  struct TestSemaphore app_lock;
  struct TestSemaphore transport_lock;
  struct TestHandle handle;
  struct TestCall first;
  struct TestCall second;
  struct TestCall foreign;
  int pop_status;
  int poster_waiting;
  int poster_saw_lock;
  int pop_saw_lock;
  int queued_saw_lock;
  int acknowledgement_saw_lock;
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
  uint32_t last_wait_mask;
};

static struct TestState *test_state;
static int failures;


static void mock_poster(void)
{
  if (test_state->app_lock.owner == TEST_OWNER_NONE) {
    test_state->handle.event_posted = 1U;
  } else {
    test_state->poster_waiting = 1;
    test_state->poster_saw_lock = 1;
  }
}

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
  if (semaphore == &test_state->app_lock && test_state->poster_waiting) {
    test_state->poster_waiting = 0;
    mock_poster();
  }
}

static void mock_service(void *opaque)
{
  struct TestState *state = (struct TestState *)opaque;

  if (state->app_lock.owner != TEST_OWNER_DISPATCHER) {
    failures++;
  }
  state->handle.heartbeat++;
}

static void mock_message_pop(void *opaque, void *message, int *status)
{
  struct TestHandle *handle = (struct TestHandle *)opaque;

  (void)message;
  if (test_state->app_lock.owner != TEST_OWNER_DISPATCHER ||
      handle != &test_state->handle) {
    failures++;
  }
  test_state->pop_saw_lock = 1;
  *status = test_state->pop_status;
}

static int mock_event_posted(void *opaque)
{
  return ((struct TestHandle *)opaque)->event_posted != 0U;
}

static int mock_event_queued(void *opaque)
{
  (void)opaque;
  if (test_state->app_lock.owner != TEST_OWNER_DISPATCHER) {
    failures++;
  }
  test_state->queued_saw_lock = 1;
  mock_poster();
  return 0;
}

static void mock_clear_event_posted(void *opaque)
{
  struct TestHandle *handle = (struct TestHandle *)opaque;

  if (test_state->app_lock.owner != TEST_OWNER_DISPATCHER) {
    failures++;
  }
  test_state->acknowledgement_saw_lock = 1;
  handle->event_posted = 0U;
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

#define ZZ9K_LIB_SYNC_OBTAIN(lock) mock_obtain(lock)
#define ZZ9K_LIB_SYNC_ATTEMPT(lock) mock_attempt(lock)
#define ZZ9K_LIB_SYNC_RELEASE(lock) mock_release(lock)
#define ZZ9K_LIB_SYNC_SERVICE(base) mock_service(base)
#define ZZ9K_LIB_SYNC_MESSAGE_POP(handle, message, status) \
  mock_message_pop(handle, message, &(status))
#define ZZ9K_LIB_SYNC_EVENT_POSTED(handle) mock_event_posted(handle)
#define ZZ9K_LIB_SYNC_EVENT_QUEUED(handle) mock_event_queued(handle)
#define ZZ9K_LIB_SYNC_CLEAR_EVENT_POSTED(handle) mock_clear_event_posted(handle)
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
#undef ZZ9K_LIB_SYNC_SERVICE
#undef ZZ9K_LIB_SYNC_MESSAGE_POP
#undef ZZ9K_LIB_SYNC_EVENT_POSTED
#undef ZZ9K_LIB_SYNC_EVENT_QUEUED
#undef ZZ9K_LIB_SYNC_CLEAR_EVENT_POSTED
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

static void test_dispatcher_services_leases_before_busy_transport(void)
{
  struct TestState state;

  init_state(&state);
  state.transport_lock.owner = TEST_OWNER_OTHER;
  expect(!zz9k_lib_dispatcher_service_apps(&state, &state.app_lock,
                                            &state.transport_lock),
         "dispatcher acquired transport held by another task");
  expect(state.handle.heartbeat == 1U,
         "dispatcher did not service app leases before busy transport");
  expect(state.transport_lock.obtains == 0U,
         "dispatcher blocked obtaining busy transport");
  expect(state.transport_lock.attempts == 1U,
         "dispatcher did not attempt busy transport exactly once");
  expect(state.app_lock.owner == TEST_OWNER_NONE,
         "dispatcher retained app lock after lease service");
}

static void test_acknowledgement_keeps_concurrent_event(void)
{
  struct TestState state;
  unsigned char message[1];

  init_state(&state);
  state.handle.event_posted = 1U;
  expect(zz9k_lib_app_pop_and_acknowledge(&state.app_lock, &state.handle,
                                           message) == 0,
         "message pop failed");
  expect(state.pop_saw_lock && state.queued_saw_lock &&
             state.acknowledgement_saw_lock,
         "message acknowledgement did not hold app lock");
  expect(state.poster_saw_lock,
         "concurrent poster ran before acknowledgement held app lock");
  expect(state.handle.event_posted == 1U,
         "acknowledgement lost concurrent posted event");
  expect(state.app_lock.owner == TEST_OWNER_NONE,
         "message acknowledgement retained app lock");
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

int main(void)
{
  test_dispatcher_services_leases_before_busy_transport();
  test_acknowledgement_keeps_concurrent_event();
  test_concurrent_waiters_route_and_wake_owners();
  test_timeout_abandons_pending_record();
  test_break_signal_remains_pending();
  test_late_completion_self_drains_on_tick();
  test_deadline_drains_late_completion();
  return failures ? 1 : 0;
}
