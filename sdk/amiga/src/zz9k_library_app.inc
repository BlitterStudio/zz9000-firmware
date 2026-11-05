/* Included by zz9k_library_resident.c: keeps all resident-base access local. */

static void zz9k_lib_app_cache(volatile uint8_t *window, uint32_t offset,
                               uint32_t length)
{
  if (window && length) {
    CacheClearE((APTR)(window + offset), length, CACRF_ClearD);
  }
}

static struct mmu_context *zz9k_lib_app_mmu_default(ZZ9KAppHandle *handle)
{
  register struct mmu_context *d0 __asm("d0");
  register struct Library *a6 __asm("a6") = handle->mmu_base;
  __asm volatile("jsr -150(a6)" : "=r"(d0) : "r"(a6) :
                 "cc", "memory", "d1", "a0", "a1");
  return d0;
}

static uint32_t zz9k_lib_app_mmu_page_size(ZZ9KAppHandle *handle)
{
  register uint32_t d0 __asm("d0");
  register struct mmu_context *a0 __asm("a0") = handle->mmu_context;
  register struct Library *a6 __asm("a6") = handle->mmu_base;
  __asm volatile("jsr -48(a6)" : "=r"(d0), "+r"(a0) : "r"(a6) :
                 "cc", "memory", "d1", "a1");
  return d0;
}

static uint32_t zz9k_lib_app_mmu_properties(ZZ9KAppHandle *handle,
                                             uint32_t address)
{
  register uint32_t d0 __asm("d0");
  register struct mmu_context *a0 __asm("a0") = handle->mmu_context;
  register void *a1 __asm("a1") = (void *)(uintptr_t)address;
  register void *a2 __asm("a2") = 0;
  register struct Library *a6 __asm("a6") = handle->mmu_base;
  __asm volatile("jsr -90(a6)" : "=r"(d0), "+r"(a0), "+r"(a1), "+r"(a2) :
                 "r"(a6) : "cc", "memory", "d1");
  return d0;
}

static int zz9k_lib_app_mmu_set_properties(ZZ9KAppHandle *handle,
                                            uint32_t flags, uint32_t mask,
                                            uint32_t address, uint32_t bytes)
{
  register uint32_t d0 __asm("d0") = bytes;
  register uint32_t d1 __asm("d1") = flags;
  register uint32_t d2 __asm("d2") = mask;
  register struct mmu_context *a0 __asm("a0") = handle->mmu_context;
  register void *a1 __asm("a1") = (void *)(uintptr_t)address;
  register void *a2 __asm("a2") = 0;
  register struct Library *a6 __asm("a6") = handle->mmu_base;
  __asm volatile("jsr -84(a6)" : "+r"(d0), "+r"(d1), "+r"(d2), "+r"(a0),
                 "+r"(a1), "+r"(a2) : "r"(a6) : "cc", "memory");
  return d0 != 0U;
}

static void zz9k_lib_app_cache_restore(ZZ9KAppHandle *handle)
{
  uint32_t i;

  if (!handle) return;
  if (handle->mmu_base && handle->mmu_context && handle->mmu_pages) {
    for (i = 0U; i < handle->mmu_page_count; i++) {
      (void)zz9k_lib_app_mmu_set_properties(handle,
          handle->mmu_pages[i].properties & ZZ9K_MMU_CACHE_MASK,
          ZZ9K_MMU_CACHE_MASK, handle->mmu_pages[i].address,
          handle->mmu_page_bytes);
    }
  }
  if (handle->mmu_pages) {
    FreeMem(handle->mmu_pages,
            (ULONG)handle->mmu_page_count * sizeof(*handle->mmu_pages));
  }
  if (handle->mmu_base) CloseLibrary(handle->mmu_base);
  handle->mmu_base = 0;
  handle->mmu_context = 0;
  handle->mmu_pages = 0;
  handle->mmu_page_count = 0U;
  handle->mmu_page_bytes = 0U;
}

static int zz9k_lib_app_cache_inhibit(ZZ9KAppHandle *handle)
{
  uint32_t page_bytes;
  uint32_t first;
  uint32_t last;
  uint32_t count;
  uint32_t i;
  uint32_t address;

  if (!handle || !handle->window) return ZZ9K_STATUS_BAD_REQUEST;
  handle->mmu_base = OpenLibrary((CONST_STRPTR)"mmu.library", 41);
  if (!handle->mmu_base) return ZZ9K_STATUS_UNSUPPORTED;
  handle->mmu_context = zz9k_lib_app_mmu_default(handle);
  page_bytes = handle->mmu_context ? zz9k_lib_app_mmu_page_size(handle) : 0U;
  if (page_bytes == 0U || (page_bytes & (page_bytes - 1U)) != 0U) {
    zz9k_lib_app_cache_restore(handle);
    return ZZ9K_STATUS_UNSUPPORTED;
  }
  first = (uint32_t)(uintptr_t)handle->window & ~(page_bytes - 1U);
  if (ZZ9K_APP_WINDOW_BYTES > UINT32_MAX - (uint32_t)(uintptr_t)handle->window ||
      (uint32_t)(uintptr_t)handle->window + ZZ9K_APP_WINDOW_BYTES >
          UINT32_MAX - (page_bytes - 1U)) {
    zz9k_lib_app_cache_restore(handle);
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  last = ((uint32_t)(uintptr_t)handle->window + ZZ9K_APP_WINDOW_BYTES +
           page_bytes - 1U) & ~(page_bytes - 1U);
  count = (last - first) / page_bytes;
  if (count == 0U || count > UINT32_MAX / sizeof(*handle->mmu_pages)) {
    zz9k_lib_app_cache_restore(handle);
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  handle->mmu_pages = (struct zz9k_mmu_page *)AllocMem(
      (ULONG)count * sizeof(*handle->mmu_pages), MEMF_PUBLIC | MEMF_CLEAR);
  if (!handle->mmu_pages) {
    zz9k_lib_app_cache_restore(handle);
    return ZZ9K_STATUS_NO_MEMORY;
  }
  handle->mmu_page_count = count;
  handle->mmu_page_bytes = page_bytes;
  for (i = 0U; i < count; i++) {
    address = first + i * page_bytes;
    handle->mmu_pages[i].address = address;
    handle->mmu_pages[i].properties = zz9k_lib_app_mmu_properties(handle,
                                                                    address);
    if (!zz9k_lib_app_mmu_set_properties(handle, ZZ9K_MMU_MAPP_CACHEINHIBIT,
                                         ZZ9K_MMU_CACHE_MASK, address,
                                         page_bytes)) {
      zz9k_lib_app_cache_restore(handle);
      return ZZ9K_STATUS_UNSUPPORTED;
    }
  }
  return ZZ9K_STATUS_OK;
}

static uint32_t zz9k_lib_app_life_get(volatile uint8_t *window, uint32_t offset)
{
  zz9k_lib_app_cache(window, ZZ9K_APP_LIFE_OFFSET + offset, 4U);
  return zz9k_get_be32((const void *)(window + ZZ9K_APP_LIFE_OFFSET + offset));
}

static void zz9k_lib_app_life_put(volatile uint8_t *window, uint32_t offset,
                                   uint32_t value)
{
  zz9k_put_be32((void *)(window + ZZ9K_APP_LIFE_OFFSET + offset), value);
  zz9k_lib_app_cache(window, ZZ9K_APP_LIFE_OFFSET + offset, 4U);
}

static int zz9k_lib_app_generation_matches(const ZZ9KAppHandle *handle)
{
  return handle && zz9k_app_generation_matches(handle->generation,
      zz9k_lib_app_life_get(handle->window, ZZ9K_APP_LIFE_GENERATION_OFFSET));
}

static int zz9k_lib_app_event_queued(const ZZ9KAppHandle *handle)
{
  struct Node *node;
  int queued = 0;

  if (!handle || !handle->notify_port) return 0;
  Forbid();
  for (node = handle->notify_port->mp_MsgList.lh_Head; node->ln_Succ;
       node = node->ln_Succ) {
    if (node == &handle->event.message.mn_Node) {
      queued = 1;
      break;
    }
  }
  Permit();
  return queued;
}

static void zz9k_lib_app_event_remove(ZZ9KAppHandle *handle)
{
  struct Node *node;

  if (!handle || !handle->event_posted || !handle->notify_port) return;
  Forbid();
  for (node = handle->notify_port->mp_MsgList.lh_Head; node->ln_Succ;
       node = node->ln_Succ) {
    if (node == &handle->event.message.mn_Node) {
      Remove(node);
      handle->event_posted = 0U;
      break;
    }
  }
  Permit();
}

static int zz9k_lib_app_valid_locked(struct ZZ9KBase *base,
                                     const ZZ9KAppHandle *handle)
{
  if (!base || !handle || base->app != handle || !handle->active) {
    return ZZ9K_STATUS_BAD_HANDLE;
  }
  if (handle->owner_task != FindTask(0)) {
    return ZZ9K_STATUS_BUSY;
  }
  return ZZ9K_STATUS_OK;
}

static int zz9k_lib_app_call_locked(struct ZZ9KBase *base, ZZ9KRequest *request,
                                    ZZ9KMailboxEntry *reply)
{
  return ZZ9KCall(&base->core, request, reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
}

static void zz9k_lib_app_fill_life_status(ZZ9KAppHandle *handle,
                                          ZZ9KAppStatusInfo *status)
{
  volatile uint8_t *life = handle->window + ZZ9K_APP_LIFE_OFFSET;

  zz9k_lib_app_cache(handle->window, ZZ9K_APP_LIFE_OFFSET,
                      ZZ9K_APP_LIFE_BYTES);
  status->state = zz9k_get_be32((const void *)(life + 8U));
  status->generation = zz9k_get_be32((const void *)(life + 12U));
  status->fault_code = zz9k_get_be32((const void *)(life + 24U));
  status->exit_code = (int32_t)zz9k_get_be32((const void *)(life + 28U));
  status->flags = zz9k_get_be32((const void *)(life + 56U));
  status->pc = zz9k_get_be32((const void *)(life + 32U));
  status->sp = zz9k_get_be32((const void *)(life + 36U));
  status->lr = zz9k_get_be32((const void *)(life + 40U));
  status->far = zz9k_get_be32((const void *)(life + 44U));
  status->fsr = zz9k_get_be32((const void *)(life + 48U));
  status->cpsr = zz9k_get_be32((const void *)(life + 52U));
  status->flip_generation = zz9k_get_be32((const void *)(life + 68U));
}

static void zz9k_lib_app_service_locked(struct ZZ9KBase *base)
{
  ZZ9KAppHandle *handle;
  ZZ9KAppStatusInfo status;
  uint32_t head;
  uint32_t tail;
  uint32_t event_type;

  handle = base ? base->app : 0;
  if (!handle || !handle->active || handle->vblank_pending == 0U) {
    return;
  }
  handle->vblank_pending = 0U;
  if (!zz9k_lib_app_generation_matches(handle)) {
    return;
  }
  handle->heartbeat++;
  if (handle->heartbeat == 0U) {
    handle->heartbeat = 1U;
  }
  zz9k_lib_app_life_put(handle->window, ZZ9K_APP_LIFE_HEARTBEAT_OFFSET,
                         handle->heartbeat);
  zz9k_lib_app_cache(handle->window, ZZ9K_APP_MESSAGE_A2H_OFFSET, 8U);
  if (zz9k_app_ring_indices(handle->window + ZZ9K_APP_MESSAGE_A2H_OFFSET,
                            &head, &tail) != ZZ9K_STATUS_OK) {
    return;
  }
  zz9k_lib_app_fill_life_status(handle, &status);
  if (!zz9k_app_generation_matches(handle->generation, status.generation)) {
    return;
  }
  if (head == tail && (!handle->started ||
      (status.state != ZZ9K_APP_STATE_IDLE &&
       status.state != ZZ9K_APP_STATE_FAULTED))) {
    return;
  }
  if (handle->signal_mask) {
    Signal(handle->owner_task, handle->signal_mask);
  }
  if (!handle->notify_port || handle->event_posted) {
    return;
  }
  event_type = head != tail ? ZZ9K_APP_EVENT_MESSAGE :
      (status.fault_code ? ZZ9K_APP_EVENT_FAULT : ZZ9K_APP_EVENT_COMPLETE);
  memset(&handle->event.message, 0, sizeof(handle->event.message));
  handle->event.message.mn_Length = (UWORD)sizeof(handle->event);
  handle->event.type = event_type;
  handle->event.status = status;
  handle->event_posted = 1U;
  PutMsg(handle->notify_port, &handle->event.message);
}

static int zz9k_lib_app_open(REG(a6, struct ZZ9KBase *base),
                             REG(a0, ZZ9KAppHandle **out),
                             REG(a1, struct MsgPort *notify_port))
{
  ZZ9KAppHandle *handle;
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  ZZ9KBoard board;
  uint32_t caps;
  uint32_t abi_major;
  uint32_t host_offset;
  uint32_t bytes;
  BYTE signal_bit;
  int status;

  if (!base || !out) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  *out = 0;
  signal_bit = AllocSignal(-1);
  if (signal_bit < 0) {
    return ZZ9K_STATUS_NO_MEMORY;
  }
  handle = (ZZ9KAppHandle *)AllocVec(sizeof(*handle), MEMF_PUBLIC | MEMF_CLEAR);
  if (!handle) {
    FreeSignal(signal_bit);
    return ZZ9K_STATUS_NO_MEMORY;
  }
  handle->owner_task = FindTask(0);
  handle->notify_port = notify_port;
  handle->signal_mask = 1UL << signal_bit;
  handle->signal_bit = signal_bit;

  status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    FreeSignal(signal_bit);
    FreeVec(handle);
    return status;
  }
  if (base->app) {
    zz9k_lib_leave(base);
    FreeSignal(signal_bit);
    FreeVec(handle);
    return ZZ9K_STATUS_BUSY;
  }
  status = zz9k_app_request_init(&request, ZZ9K_APP_OP_QUERY);
  if (status == ZZ9K_STATUS_OK) {
    status = zz9k_lib_app_call_locked(base, &request, &reply);
  }
  if (status != ZZ9K_STATUS_OK || reply.payload_len < ZZ9K_APP_QUERY_REPLY_BYTES) {
    zz9k_lib_leave(base);
    FreeSignal(signal_bit);
    FreeVec(handle);
    return status == ZZ9K_STATUS_OK ? ZZ9K_STATUS_UNSUPPORTED : status;
  }
  abi_major = zz9k_get_be32(reply.payload.inline_data);
  caps = zz9k_get_be32(reply.payload.inline_data + 12U);
  if (abi_major != ZZ9K_APP_ABI_MAJOR || !(caps & ZZ9K_APP_CAP_Z3) ||
      zz9k_get_be32(reply.payload.inline_data + 8U) != ZZ9K_APP_STATE_IDLE) {
    zz9k_lib_leave(base);
    FreeSignal(signal_bit);
    FreeVec(handle);
    return (caps & ZZ9K_APP_CAP_Z3) ? ZZ9K_STATUS_BUSY : ZZ9K_STATUS_UNSUPPORTED;
  }
  status = zz9k_app_request_init(&request, ZZ9K_APP_OP_ALLOC_WINDOW);
  if (status == ZZ9K_STATUS_OK) {
    status = zz9k_lib_app_call_locked(base, &request, &reply);
  }
  if (status != ZZ9K_STATUS_OK || reply.payload_len < ZZ9K_APP_ALLOC_REPLY_BYTES ||
      !zz9k_find_board(&board)) {
    zz9k_lib_leave(base);
    FreeSignal(signal_bit);
    FreeVec(handle);
    return status == ZZ9K_STATUS_OK ? ZZ9K_STATUS_IO_ERROR : status;
  }
  bytes = zz9k_get_be32(reply.payload.inline_data + 4U);
  host_offset = zz9k_get_be32(reply.payload.inline_data + 8U);
  if (board.zorro_version != 3U || bytes < ZZ9K_APP_WINDOW_BYTES ||
      host_offset > board.board_size || bytes > board.board_size - host_offset) {
    zz9k_lib_leave(base);
    FreeSignal(signal_bit);
    FreeVec(handle);
    return ZZ9K_STATUS_UNSUPPORTED;
  }
  handle->window = (volatile uint8_t *)(uintptr_t)(board.board_addr + host_offset);
  status = zz9k_lib_app_cache_inhibit(handle);
  if (status != ZZ9K_STATUS_OK) {
    zz9k_lib_leave(base);
    FreeSignal(signal_bit);
    FreeVec(handle);
    return status;
  }
  if (zz9k_lib_app_life_get(handle->window, 0U) != ZZ9K_APP_LIFE_MAGIC ||
      zz9k_lib_app_life_get(handle->window, 4U) != ZZ9K_APP_LIFE_VERSION) {
    zz9k_lib_app_cache_restore(handle);
    zz9k_lib_leave(base);
    FreeSignal(signal_bit);
    FreeVec(handle);
    return ZZ9K_STATUS_INTERNAL_ERROR;
  }
  handle->active = 1U;
  base->app = handle;
  zz9k_lib_leave(base);
  status = zz9k_lib_dispatcher_ensure(base);
  if (status != ZZ9K_STATUS_OK) {
    (void)zz9k_lib_enter(base);
    if (base->app == handle) base->app = 0;
    zz9k_lib_leave(base);
    zz9k_lib_app_cache_restore(handle);
    FreeSignal(signal_bit);
    FreeVec(handle);
    return status;
  }
  *out = handle;
  return ZZ9K_STATUS_OK;
}

static int zz9k_lib_app_load_begin_locked(struct ZZ9KBase *base,
                                          ZZ9KAppHandle *handle)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  status = zz9k_app_request_init(&request, ZZ9K_APP_OP_LOAD_BEGIN);
  if (status != ZZ9K_STATUS_OK) return status;
  status = zz9k_lib_app_call_locked(base, &request, &reply);
  if (status == ZZ9K_STATUS_OK) {
    handle->generation = zz9k_lib_app_life_get(handle->window,
                                                ZZ9K_APP_LIFE_GENERATION_OFFSET);
    handle->started = 0U;
  }
  return status;
}

static int zz9k_lib_app_load_end_locked(struct ZZ9KBase *base)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status = zz9k_app_request_init(&request, ZZ9K_APP_OP_LOAD_END);
  return status == ZZ9K_STATUS_OK ? zz9k_lib_app_call_locked(base, &request, &reply)
                                  : status;
}

static int zz9k_lib_app_load_memory(REG(a6, struct ZZ9KBase *base),
                                    REG(a0, ZZ9KAppHandle *handle),
                                    REG(a1, const void *data),
                                    REG(d0, uint32_t length))
{
  const uint8_t *source = (const uint8_t *)data;
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  uint32_t offset = 0U;
  int status;

  if (!data || length == 0U || length > ZZ9K_APP_IMAGE_MAX_BYTES) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) return status;
  status = zz9k_lib_app_valid_locked(base, handle);
  if (status == ZZ9K_STATUS_OK) status = zz9k_lib_app_load_begin_locked(base, handle);
  while (status == ZZ9K_STATUS_OK && offset < length) {
    uint32_t chunk = length - offset;
    if (chunk > ZZ9K_APP_CHUNK_MAX) chunk = ZZ9K_APP_CHUNK_MAX;
    memcpy((void *)(handle->window + ZZ9K_APP_STAGING_OFFSET), source + offset,
           chunk);
    zz9k_lib_app_cache(handle->window, ZZ9K_APP_STAGING_OFFSET, chunk);
    status = zz9k_app_request_chunk(&request, offset, chunk);
    if (status == ZZ9K_STATUS_OK) status = zz9k_lib_app_call_locked(base, &request, &reply);
    offset += chunk;
  }
  if (status == ZZ9K_STATUS_OK) status = zz9k_lib_app_load_end_locked(base);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_app_load_file(REG(a6, struct ZZ9KBase *base),
                                  REG(a0, ZZ9KAppHandle *handle),
                                  REG(a1, const char *path))
{
  BPTR file;
  LONG size;
  LONG got;
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  uint32_t offset = 0U;
  int status;

  if (!path) return ZZ9K_STATUS_BAD_REQUEST;
  file = Open((STRPTR)path, MODE_OLDFILE);
  if (!file) return ZZ9K_STATUS_NOT_FOUND;
  (void)Seek(file, 0L, OFFSET_END);
  size = Seek(file, 0L, OFFSET_BEGINNING);
  if (size <= 0L || (uint32_t)size > ZZ9K_APP_IMAGE_MAX_BYTES) {
    Close(file);
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) {
    Close(file);
    return status;
  }
  status = zz9k_lib_app_valid_locked(base, handle);
  if (status == ZZ9K_STATUS_OK) status = zz9k_lib_app_load_begin_locked(base, handle);
  while (status == ZZ9K_STATUS_OK && offset < (uint32_t)size) {
    uint32_t chunk = (uint32_t)size - offset;
    if (chunk > ZZ9K_APP_CHUNK_MAX) chunk = ZZ9K_APP_CHUNK_MAX;
    got = Read(file, (APTR)(handle->window + ZZ9K_APP_STAGING_OFFSET), (LONG)chunk);
    if (got != (LONG)chunk) {
      status = ZZ9K_STATUS_IO_ERROR;
      break;
    }
    zz9k_lib_app_cache(handle->window, ZZ9K_APP_STAGING_OFFSET, chunk);
    status = zz9k_app_request_chunk(&request, offset, chunk);
    if (status == ZZ9K_STATUS_OK) status = zz9k_lib_app_call_locked(base, &request, &reply);
    offset += chunk;
  }
  if (status == ZZ9K_STATUS_OK) status = zz9k_lib_app_load_end_locked(base);
  zz9k_lib_leave(base);
  Close(file);
  return status;
}

static int zz9k_lib_app_start(REG(a6, struct ZZ9KBase *base),
                              REG(a0, ZZ9KAppHandle *handle),
                              REG(a1, const char *const *argv),
                              REG(d0, uint32_t argc))
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  uint8_t *area;
  uint32_t at = ZZ9K_APP_ARGV_STRINGS_OFFSET;
  uint32_t i;
  int status;

  if (argc > ZZ9K_APP_ARGV_MAX || (argc && !argv)) return ZZ9K_STATUS_BAD_REQUEST;
  status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) return status;
  status = zz9k_lib_app_valid_locked(base, handle);
  if (status == ZZ9K_STATUS_OK && !zz9k_lib_app_generation_matches(handle)) {
    status = ZZ9K_STATUS_BAD_HANDLE;
  }
  if (status == ZZ9K_STATUS_OK) {
    area = (uint8_t *)(handle->window + ZZ9K_APP_ARGV_OFFSET);
    memset(area, 0, ZZ9K_APP_ARGV_BYTES);
    zz9k_put_be32(area, argc);
    for (i = 0U; i < argc && status == ZZ9K_STATUS_OK; i++) {
      uint32_t length;
      if (!argv[i]) {
        status = ZZ9K_STATUS_BAD_REQUEST;
        break;
      }
      length = (uint32_t)strlen(argv[i]);
      if (length >= ZZ9K_APP_ARGV_STRING_MAX || at + length + 1U > ZZ9K_APP_ARGV_BYTES) {
        status = ZZ9K_STATUS_BAD_REQUEST;
        break;
      }
      zz9k_put_be32(area + ZZ9K_APP_ARGV_OFFSETS_OFFSET + i * 4U,
                    ZZ9K_APP_ARGV_OFFSET + at);
      memcpy(area + at, argv[i], length + 1U);
      at += length + 1U;
    }
    zz9k_lib_app_cache(handle->window, ZZ9K_APP_ARGV_OFFSET, ZZ9K_APP_ARGV_BYTES);
  }
  if (status == ZZ9K_STATUS_OK) status = zz9k_app_request_start(&request, argc);
  if (status == ZZ9K_STATUS_OK) status = zz9k_lib_app_call_locked(base, &request, &reply);
  if (status == ZZ9K_STATUS_OK) handle->started = 1U;
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_app_stop(REG(a6, struct ZZ9KBase *base),
                             REG(a0, ZZ9KAppHandle *handle))
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) return status;
  status = zz9k_lib_app_valid_locked(base, handle);
  if (status == ZZ9K_STATUS_OK && !zz9k_lib_app_generation_matches(handle)) {
    status = ZZ9K_STATUS_BAD_HANDLE;
  }
  if (status == ZZ9K_STATUS_OK) status = zz9k_app_request_init(&request, ZZ9K_APP_OP_STOP);
  if (status == ZZ9K_STATUS_OK) status = zz9k_lib_app_call_locked(base, &request, &reply);
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_app_status(REG(a6, struct ZZ9KBase *base),
                               REG(a0, ZZ9KAppHandle *handle),
                               REG(a1, ZZ9KAppStatusInfo *status_out))
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;
  if (!status_out) return ZZ9K_STATUS_BAD_REQUEST;
  status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) return status;
  status = zz9k_lib_app_valid_locked(base, handle);
  if (status == ZZ9K_STATUS_OK && !zz9k_lib_app_generation_matches(handle)) {
    status = ZZ9K_STATUS_BAD_HANDLE;
  }
  if (status == ZZ9K_STATUS_OK) status = zz9k_app_request_init(&request, ZZ9K_APP_OP_STATUS);
  if (status == ZZ9K_STATUS_OK) status = zz9k_lib_app_call_locked(base, &request, &reply);
  if (status == ZZ9K_STATUS_OK) status = zz9k_app_parse_status(&reply, status_out);
  if (status == ZZ9K_STATUS_OK &&
      !zz9k_app_generation_matches(handle->generation, status_out->generation)) {
    status = ZZ9K_STATUS_BAD_HANDLE;
  }
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_app_send_message(REG(a6, struct ZZ9KBase *base),
                                     REG(a0, ZZ9KAppHandle *handle),
                                     REG(a1, const ZZ9KAppMessage *message))
{
  int status;
  if (!message) return ZZ9K_STATUS_BAD_REQUEST;
  status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) return status;
  status = zz9k_lib_app_valid_locked(base, handle);
  if (status == ZZ9K_STATUS_OK && !zz9k_lib_app_generation_matches(handle)) {
    status = ZZ9K_STATUS_BAD_HANDLE;
  }
  if (status == ZZ9K_STATUS_OK) {
    zz9k_lib_app_cache(handle->window, ZZ9K_APP_MESSAGE_H2A_OFFSET,
                        ZZ9K_APP_MESSAGE_RING_BYTES);
    status = zz9k_app_ring_push(handle->window + ZZ9K_APP_MESSAGE_H2A_OFFSET,
                                message->bytes);
    if (status == ZZ9K_STATUS_OK) zz9k_lib_app_cache(handle->window,
        ZZ9K_APP_MESSAGE_H2A_OFFSET, ZZ9K_APP_MESSAGE_RING_BYTES);
  }
  zz9k_lib_leave(base);
  return status;
}

static int zz9k_lib_app_get_message(REG(a6, struct ZZ9KBase *base),
                                    REG(a0, ZZ9KAppHandle *handle),
                                    REG(a1, ZZ9KAppMessage *message))
{
  int status;
  if (!message) return ZZ9K_STATUS_BAD_REQUEST;
  status = zz9k_lib_enter(base);
  if (status != ZZ9K_STATUS_OK) return status;
  status = zz9k_lib_app_valid_locked(base, handle);
  if (status == ZZ9K_STATUS_OK && !zz9k_lib_app_generation_matches(handle)) {
    status = ZZ9K_STATUS_BAD_HANDLE;
  }
  if (status == ZZ9K_STATUS_OK) {
    zz9k_lib_app_cache(handle->window, ZZ9K_APP_MESSAGE_A2H_OFFSET,
                        ZZ9K_APP_MESSAGE_RING_BYTES);
    status = zz9k_app_ring_pop(handle->window + ZZ9K_APP_MESSAGE_A2H_OFFSET,
                               message->bytes);
    if (status == ZZ9K_STATUS_OK) zz9k_lib_app_cache(handle->window,
        ZZ9K_APP_MESSAGE_A2H_OFFSET, ZZ9K_APP_MESSAGE_RING_BYTES);
    if (handle->event_posted && !zz9k_lib_app_event_queued(handle)) {
      handle->event_posted = 0U;
    }
  }
  zz9k_lib_leave(base);
  return status;
}

static ULONG zz9k_lib_app_message_signal(REG(a6, struct ZZ9KBase *base),
                                         REG(a0, const ZZ9KAppHandle *handle))
{
  ULONG signal = 0UL;
  if (zz9k_lib_enter(base) == ZZ9K_STATUS_OK) {
    if (zz9k_lib_app_valid_locked(base, handle) == ZZ9K_STATUS_OK &&
        zz9k_lib_app_generation_matches(handle)) {
      signal = handle->signal_mask;
    }
    zz9k_lib_leave(base);
  }
  return signal;
}

static void zz9k_lib_app_destroy_locked(struct ZZ9KBase *base,
                                        ZZ9KAppHandle *handle)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;

  if (!handle) return;
  if (base && base->core.ctx && handle->active &&
      zz9k_lib_app_generation_matches(handle) &&
      zz9k_app_request_init(&request, ZZ9K_APP_OP_STOP) == ZZ9K_STATUS_OK) {
    (void)zz9k_lib_app_call_locked(base, &request, &reply);
  }
  zz9k_lib_app_event_remove(handle);
  if (base && base->app == handle) base->app = 0;
  handle->active = 0U;
  zz9k_lib_app_cache_restore(handle);
  if (handle->owner_task == FindTask(0)) {
    if (handle->signal_bit >= 0) FreeSignal(handle->signal_bit);
    FreeVec(handle);
  }
}

static void zz9k_lib_app_close(REG(a6, struct ZZ9KBase *base),
                               REG(a0, ZZ9KAppHandle *handle))
{
  if (zz9k_lib_enter(base) != ZZ9K_STATUS_OK) return;
  if (zz9k_lib_app_valid_locked(base, handle) == ZZ9K_STATUS_OK) {
    zz9k_lib_app_destroy_locked(base, handle);
  }
  zz9k_lib_leave(base);
}

static void zz9k_lib_app_close_all_locked(struct ZZ9KBase *base)
{
  if (base && base->app) zz9k_lib_app_destroy_locked(base, base->app);
}
