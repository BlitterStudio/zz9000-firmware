/*
 * Print basic SDK v2 firmware capabilities and diagnostics.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/caps.h"
#include "zz9k/host.h"
#include <stdio.h>

static void print_capability_name(int *first, const char *name)
{
  printf("%s%s", *first ? "" : ",", name);
  *first = 0;
}

static void print_capability_names(uint32_t capability_bits)
{
  uint32_t remaining;
  uint32_t i;
  uint32_t count;
  int first = 1;

  printf("Capability names:     ");
  remaining = capability_bits;
  count = zz9k_known_capability_count();
  for (i = 0; i < count; i++) {
    const char *name;
    uint32_t bit;

    bit = zz9k_known_capability_bit(i);
    if (bit == 0U || (capability_bits & bit) == 0U) {
      continue;
    }

    name = zz9k_capability_name(bit);
    if (name) {
      print_capability_name(&first, name);
      remaining &= ~bit;
    }
  }
  if (remaining != 0U) {
    printf("%s0x%08lx", first ? "" : ",", (unsigned long)remaining);
    first = 0;
  }
  if (first) {
    printf("none");
  }
  printf("\n");
}

static void print_caps(const ZZ9KCaps *caps)
{
  printf("SDK ABI:              %u.%u\n",
         (unsigned)caps->abi_major, (unsigned)caps->abi_minor);
  printf("Capabilities:         0x%08lx\n",
         (unsigned long)caps->capability_bits);
  print_capability_names(caps->capability_bits);
  printf("Transport:            %s%s%s\n",
         (caps->capability_bits & ZZ9K_CAP_POLLING_COMPLETION) ?
             "polling" : "none",
         (caps->capability_bits & ZZ9K_CAP_DOORBELL) ?
             " doorbell" : "",
         (caps->capability_bits & ZZ9K_CAP_IRQ_COMPLETION) ?
             " irq" : "");
  printf("Inline payload:       %lu bytes\n",
         (unsigned long)caps->max_inline_payload);
  printf("Shared buffers:       %lu\n",
         (unsigned long)caps->max_shared_buffers);
  printf("Surfaces:             %lu\n", (unsigned long)caps->max_surfaces);
  printf("Request ring entries: %lu\n",
         (unsigned long)caps->request_ring_entries);
  printf("Completion entries:   %lu\n",
         (unsigned long)caps->completion_ring_entries);
}

static void print_aperture_layout(const ZZ9KApertureLayout *layout)
{
  printf("Aperture layout:      generation %lu flags=0x%04lx\n",
         (unsigned long)(layout->profile >>
                         ZZ9K_APERTURE_LAYOUT_GENERATION_SHIFT),
         (unsigned long)(layout->profile & ZZ9K_APERTURE_LAYOUT_FLAGS_MASK));
  printf("  Aperture:          0x%08lx bytes\n",
         (unsigned long)layout->aperture_size);
  printf("  Framebuffer:       0x%08lx + 0x%08lx\n",
         (unsigned long)layout->framebuffer_base,
         (unsigned long)layout->framebuffer_size);
  printf("  PIP source pool:   0x%08lx + 0x%08lx\n",
         (unsigned long)layout->pip_base,
         (unsigned long)layout->pip_size);
  printf("  Template staging:  0x%08lx + 0x%08lx\n",
         (unsigned long)layout->template_base,
         (unsigned long)layout->template_size);
  printf("  Host window heap:  0x%08lx + 0x%08lx\n",
         (unsigned long)layout->host_base,
         (unsigned long)layout->host_size);
  printf("  Audio buffers:     0x%08lx + 0x%08lx\n",
         (unsigned long)layout->audio_base,
         (unsigned long)layout->audio_size);
}

static void print_services(ZZ9KContext *ctx, const ZZ9KCaps *caps)
{
  uint32_t i;
  uint32_t count;

  if ((caps->capability_bits & ZZ9K_CAP_SERVICE_DISCOVERY) == 0) {
    printf("Service discovery:    unavailable\n");
    return;
  }

  printf("Services:\n");
  count = zz9k_known_service_count();
  for (i = 0; i < count; i++) {
    ZZ9KServiceInfo service;
    const char *name;
    uint32_t service_id;
    int status;

    service_id = zz9k_known_service_id(i);
    if (!zz9k_service_advertised_by_caps(service_id,
                                         caps->capability_bits)) {
      continue;
    }

    status = zz9k_query_service(ctx, service_id, &service);
    if (status == ZZ9K_STATUS_NOT_FOUND ||
        status == ZZ9K_STATUS_UNSUPPORTED) {
      continue;
    }
    if (status != ZZ9K_STATUS_OK) {
      printf("  %-10s query failed: %s (%d)\n",
             zz9k_service_name(service_id), zz9k_status_name(status), status);
      continue;
    }

    name = service.name[0] ? service.name : zz9k_service_name(service_id);
    printf("  %-10s id=0x%04lx version=%lu.%lu caps=0x%08lx ops=%lu\n",
           name,
           (unsigned long)service.service_id,
           (unsigned long)((service.version >> 16) & 0xffffUL),
           (unsigned long)(service.version & 0xffffUL),
           (unsigned long)service.capability_bits,
           (unsigned long)service.opcode_count);
  }
}

static void print_diag(const ZZ9KDiagInfo *diag)
{
  printf("Mailbox address:      0x%08lx\n",
         (unsigned long)diag->mailbox_arm_addr);
  printf("Mailbox entries:      %lu\n",
         (unsigned long)diag->mailbox_ring_entries);
  printf("Requests completed:   %lu\n",
         (unsigned long)diag->requests_completed);
  printf("Requests failed:      %lu\n",
         (unsigned long)diag->requests_failed);
  printf("Pending requests:     %lu\n",
         (unsigned long)diag->pending_requests);
  printf("Last status:          %s (%lu)\n",
         zz9k_status_name((int)diag->last_status),
         (unsigned long)diag->last_status);
  printf("Shared buffers used:  %lu\n",
         (unsigned long)diag->shared_buffers_used);
  printf("Surfaces used:        %lu\n",
         (unsigned long)diag->surfaces_used);
  printf("Invalid alloc slots:  %lu\n",
         (unsigned long)diag->allocator_invalid_slots);
  printf("Shared heap total:    %lu bytes\n",
         (unsigned long)diag->shared_heap_total);
  printf("Shared heap free:     %lu bytes\n",
         (unsigned long)diag->shared_heap_free);
  printf("Largest free block:   %lu bytes\n",
         (unsigned long)diag->shared_heap_largest_free);
}

static void print_diag_timing(const ZZ9KDiagTimingInfo *timing)
{
  uint32_t other_requests;
  uint32_t other_us;

  other_requests = timing->requests_timed;
  if (other_requests >= timing->surface_requests) {
    other_requests -= timing->surface_requests;
  } else {
    other_requests = 0U;
  }
  if (other_requests >= timing->audio_requests) {
    other_requests -= timing->audio_requests;
  } else {
    other_requests = 0U;
  }

  other_us = timing->total_us;
  if (other_us >= timing->surface_us) {
    other_us -= timing->surface_us;
  } else {
    other_us = 0U;
  }
  if (other_us >= timing->audio_us) {
    other_us -= timing->audio_us;
  } else {
    other_us = 0U;
  }

  printf("Mailbox timing:\n");
  printf("  Timer:             %lu Hz\n",
         (unsigned long)timing->timer_hz);
  printf("  Requests timed:    %lu\n",
         (unsigned long)timing->requests_timed);
  printf("  Total service:     %lu us\n",
         (unsigned long)timing->total_us);
  printf("  Surface requests:  %lu, %lu us\n",
         (unsigned long)timing->surface_requests,
         (unsigned long)timing->surface_us);
  printf("  Audio requests:    %lu, %lu us\n",
         (unsigned long)timing->audio_requests,
         (unsigned long)timing->audio_us);
  printf("  Other requests:    %lu, %lu us\n",
         (unsigned long)other_requests, (unsigned long)other_us);
  printf("  Last request:      opcode=0x%04lx %lu us\n",
         (unsigned long)timing->last_opcode,
         (unsigned long)timing->last_us);
  printf("  Max request:       opcode=0x%04lx %lu us\n",
         (unsigned long)timing->max_opcode,
         (unsigned long)timing->max_us);
}

static void print_diag_sched(const ZZ9KDiagSchedInfo *sched)
{
  if (sched->core1_online) {
    printf("Scheduler:         core 1 = online, offload tasks core1=%lu core0=%lu\n",
           (unsigned long)sched->tasks_on_core1,
           (unsigned long)sched->tasks_on_core0);
  } else {
    printf("Scheduler:         core 1 = offline (single-core fallback)\n");
  }
  if (sched->version >= 2U) {
    printf("  Decode offloads:   %lu requests, %lu us\n",
           (unsigned long)sched->decode_requests,
           (unsigned long)sched->decode_us);
  }
}

static const char *layout_state_name(uint32_t state)
{
  switch (state) {
  case ZZ9K_APERTURE_LAYOUT_LEGACY:
    return "legacy";
  case ZZ9K_APERTURE_LAYOUT_UNACKNOWLEDGED:
    return "unacknowledged";
  case ZZ9K_APERTURE_LAYOUT_ACTIVE:
    return "active";
  case ZZ9K_APERTURE_LAYOUT_INVALID:
    return "invalid";
  default:
    return "unknown";
  }
}

static void print_diag_memory(const ZZ9KDiagMemoryInfo *memory)
{
  printf("Aperture memory:      state=%s (%lu) size=0x%08lx info=0x%08lx\n",
         layout_state_name(memory->layout_state),
         (unsigned long)memory->layout_state,
         (unsigned long)memory->aperture_size,
         (unsigned long)memory->aperture_info);
  printf("  Host heap bases:   board=0x%08lx ARM=0x%08lx\n",
         (unsigned long)memory->host_board_base,
         (unsigned long)memory->host_arm_base);
  printf("  Host heap usage:   total=%lu free=%lu largest=%lu allocations=%lu\n",
         (unsigned long)memory->host_total,
         (unsigned long)memory->host_free,
         (unsigned long)memory->host_largest_free,
         (unsigned long)memory->allocations);
  printf("  Invalid host slots: %lu\n",
         (unsigned long)memory->allocator_invalid_slots);
}

int main(void)
{
  ZZ9KContext *ctx = 0;
  ZZ9KCaps caps;
  ZZ9KDiagInfo diag;
  ZZ9KDiagTimingInfo timing;
  ZZ9KDiagSchedInfo sched;
  ZZ9KApertureLayout layout;
  ZZ9KDiagMemoryInfo memory;
  int status;

  printf("zz9k-info: opening SDK mailbox\n");
  fflush(stdout);
  status = zz9k_open(&ctx);
  if (status != ZZ9K_STATUS_OK) {
    printf("zz9k-info: open failed: %s (%d)\n",
           zz9k_status_name(status), status);
    return 1;
  }

  printf("zz9k-info: querying capabilities\n");
  fflush(stdout);
  status = zz9k_query_caps(ctx, &caps);
  if (status != ZZ9K_STATUS_OK) {
    printf("zz9k-info: query caps failed: %s (%d)\n",
           zz9k_status_name(status), status);
    zz9k_close(ctx);
    return 1;
  }

  print_caps(&caps);
  if ((caps.capability_bits & ZZ9K_CAP_APERTURE_LAYOUT) != 0U) {
    status = zz9k_query_aperture_layout(ctx, &layout);
    if (status == ZZ9K_STATUS_OK) {
      print_aperture_layout(&layout);
    } else if (status != ZZ9K_STATUS_UNSUPPORTED) {
      printf("Aperture layout:      %s (%d)\n",
             zz9k_status_name(status), status);
    }
  }
  print_services(ctx, &caps);

  printf("zz9k-info: reading diagnostics\n");
  fflush(stdout);
  status = zz9k_read_diag(ctx, &diag);
  if (status == ZZ9K_STATUS_OK) {
    print_diag(&diag);
  } else {
    printf("Diagnostics:          %s (%d)\n",
           zz9k_status_name(status), status);
  }

  status = zz9k_read_diag_timing(ctx, &timing);
  if (status == ZZ9K_STATUS_OK) {
    print_diag_timing(&timing);
  } else if (status != ZZ9K_STATUS_UNSUPPORTED) {
    printf("Timing diagnostics:   %s (%d)\n",
           zz9k_status_name(status), status);
  }

  status = zz9k_read_diag_sched(ctx, &sched);
  if (status == ZZ9K_STATUS_OK) {
    print_diag_sched(&sched);
  } else if (status != ZZ9K_STATUS_UNSUPPORTED) {
    printf("Scheduler diagnostics: %s (%d)\n",
           zz9k_status_name(status), status);
  }

  status = zz9k_read_diag_memory(ctx, &memory);
  if (status == ZZ9K_STATUS_OK) {
    print_diag_memory(&memory);
  } else if (status != ZZ9K_STATUS_UNSUPPORTED) {
    printf("Memory diagnostics:   %s (%d)\n",
           zz9k_status_name(status), status);
  }

  zz9k_close(ctx);
  return 0;
}
