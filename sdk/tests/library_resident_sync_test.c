/*
 * Executable lock-order checks for zz9k.library resident synchronization.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
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

struct TestState {
  struct TestSemaphore app_lock;
  struct TestSemaphore transport_lock;
  struct TestHandle handle;
  int pop_status;
  int poster_waiting;
  int poster_saw_lock;
  int pop_saw_lock;
  int queued_saw_lock;
  int acknowledgement_saw_lock;
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

#define ZZ9K_LIB_SYNC_OBTAIN(lock) mock_obtain(lock)
#define ZZ9K_LIB_SYNC_ATTEMPT(lock) mock_attempt(lock)
#define ZZ9K_LIB_SYNC_RELEASE(lock) mock_release(lock)
#define ZZ9K_LIB_SYNC_SERVICE(base) mock_service(base)
#define ZZ9K_LIB_SYNC_MESSAGE_POP(handle, message, status) \
  mock_message_pop(handle, message, &(status))
#define ZZ9K_LIB_SYNC_EVENT_POSTED(handle) mock_event_posted(handle)
#define ZZ9K_LIB_SYNC_EVENT_QUEUED(handle) mock_event_queued(handle)
#define ZZ9K_LIB_SYNC_CLEAR_EVENT_POSTED(handle) mock_clear_event_posted(handle)
#include "zz9k_library_resident_sync.h"
#undef ZZ9K_LIB_SYNC_OBTAIN
#undef ZZ9K_LIB_SYNC_ATTEMPT
#undef ZZ9K_LIB_SYNC_RELEASE
#undef ZZ9K_LIB_SYNC_SERVICE
#undef ZZ9K_LIB_SYNC_MESSAGE_POP
#undef ZZ9K_LIB_SYNC_EVENT_POSTED
#undef ZZ9K_LIB_SYNC_EVENT_QUEUED
#undef ZZ9K_LIB_SYNC_CLEAR_EVENT_POSTED

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

int main(void)
{
  test_dispatcher_services_leases_before_busy_transport();
  test_acknowledgement_keeps_concurrent_event();
  return failures ? 1 : 0;
}
