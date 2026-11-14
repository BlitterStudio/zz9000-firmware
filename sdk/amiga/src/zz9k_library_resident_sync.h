/*
 * Shared resident synchronization steps, parameterized by their Exec bindings.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZ9K_LIBRARY_RESIDENT_SYNC_H
#define ZZ9K_LIBRARY_RESIDENT_SYNC_H

#if !defined(ZZ9K_LIB_SYNC_OBTAIN) || !defined(ZZ9K_LIB_SYNC_ATTEMPT) || \
    !defined(ZZ9K_LIB_SYNC_RELEASE) || !defined(ZZ9K_LIB_SYNC_SERVICE) || \
    !defined(ZZ9K_LIB_SYNC_MESSAGE_POP) || \
    !defined(ZZ9K_LIB_SYNC_EVENT_POSTED) || \
    !defined(ZZ9K_LIB_SYNC_EVENT_QUEUED) || \
    !defined(ZZ9K_LIB_SYNC_CLEAR_EVENT_POSTED)
#error "resident synchronization bindings are required"
#endif

static inline int zz9k_lib_dispatcher_service_apps(void *base, void *app_lock,
                                                    void *transport_lock)
{
  ZZ9K_LIB_SYNC_OBTAIN(app_lock);
  ZZ9K_LIB_SYNC_SERVICE(base);
  ZZ9K_LIB_SYNC_RELEASE(app_lock);
  return ZZ9K_LIB_SYNC_ATTEMPT(transport_lock);
}

static inline int zz9k_lib_app_pop_and_acknowledge(void *app_lock,
                                                    void *handle,
                                                    void *message)
{
  int status;

  ZZ9K_LIB_SYNC_OBTAIN(app_lock);
  ZZ9K_LIB_SYNC_MESSAGE_POP(handle, message, status);
  if (ZZ9K_LIB_SYNC_EVENT_POSTED(handle) &&
      !ZZ9K_LIB_SYNC_EVENT_QUEUED(handle)) {
    ZZ9K_LIB_SYNC_CLEAR_EVENT_POSTED(handle);
  }
  ZZ9K_LIB_SYNC_RELEASE(app_lock);
  return status;
}

#endif
