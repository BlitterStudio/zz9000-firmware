/*
 * Logic and behavioral checks for zz9k-view WebP decode bridge.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k-webp-view.h"
#include "zz9k-picture-viewer.h"
#include "zz9k/abi.h"
#include "zz9k/caps.h"
#include "zz9k/host.h"
#include "zz9k/image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#define MOCK_BOARD_ADDR 0x20000000UL
#define MOCK_RING_ENTRIES 8U

/*
 * Fixtures from test/image/webp_fixtures.h
 */
static const uint8_t webp_lossy_3x5[] = {
  0x52, 0x49, 0x46, 0x46, 0x64, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50,
  0x56, 0x50, 0x38, 0x20, 0x58, 0x00, 0x00, 0x00, 0xf0, 0x02, 0x00, 0x9d,
  0x01, 0x2a, 0x03, 0x00, 0x05, 0x00, 0x02, 0x00, 0x34, 0x25, 0xb0, 0x02,
  0x74, 0x7f, 0x08, 0xb0, 0x07, 0x9b, 0x07, 0xe9, 0x9f, 0xb0, 0x00, 0x33,
  0x2a, 0xc4, 0x8e, 0xc0, 0x00, 0xe2, 0x79, 0xb9, 0xdd, 0x3e, 0xe0, 0x2a,
  0x7f, 0x29, 0xc9, 0x94, 0x1b, 0xd2, 0x61, 0x67, 0xd4, 0xa2, 0xd8, 0x1d,
  0x32, 0x09, 0x8f, 0xde, 0xfe, 0x7d, 0x0f, 0xfc, 0xd5, 0x81, 0xff, 0xcf,
  0xff, 0x93, 0x7f, 0xac, 0xca, 0x5d, 0xf8, 0xef, 0x8f, 0xb9, 0x59, 0xd2,
  0xfc, 0x13, 0x7b, 0x92, 0x26, 0xbf, 0xf9, 0x6f, 0xd7, 0xa0, 0x00, 0x00
};

static const uint8_t webp_lossy_3x5_bgra[] = {
  0xb6, 0x41, 0x73, 0xff, 0x91, 0x63, 0x64, 0xff, 0x4d, 0xad, 0x47, 0xff,
  0xa7, 0x60, 0x89, 0xff, 0x62, 0x54, 0x4d, 0xff, 0x6a, 0xd1, 0x6a, 0xff,
  0x6d, 0x83, 0x99, 0xff, 0x60, 0x98, 0x81, 0xff, 0x3a, 0xb1, 0x43, 0xff,
  0x70, 0x98, 0xbd, 0xff, 0x4a, 0x78, 0x7e, 0xff, 0x81, 0xbc, 0x87, 0xff,
  0x95, 0x81, 0xd7, 0xff, 0x67, 0x41, 0x90, 0xff, 0xaa, 0x5f, 0xa2, 0xff
};

static const uint8_t webp_alpha_3x4[] = {
  0x52, 0x49, 0x46, 0x46, 0x62, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50,
  0x56, 0x50, 0x38, 0x4c, 0x55, 0x00, 0x00, 0x00, 0x2f, 0x02, 0xc0, 0x00,
  0x10, 0x57, 0x40, 0x20, 0x40, 0x91, 0x47, 0x36, 0xf8, 0x7f, 0xc2, 0x11,
  0x6e, 0x10, 0x6a, 0x60, 0x9c, 0x4d, 0x20, 0x01, 0xf4, 0x7f, 0x6b, 0x84,
  0x4c, 0xc0, 0x62, 0x1a, 0x89, 0xa1, 0x7f, 0x13, 0x9f, 0x8f, 0xd9, 0x06,
  0x64, 0xc4, 0x53, 0x46, 0xb3, 0xaf, 0x39, 0xac, 0x69, 0x00, 0xe0, 0xfc,
  0x01, 0xe2, 0xbb, 0x54, 0x78, 0xbb, 0x3a, 0x4d, 0xe6, 0xc9, 0x31, 0x1d,
  0x1f, 0x14, 0xb4, 0x91, 0xc2, 0x3c, 0x7b, 0xb8, 0xc2, 0x02, 0x0a, 0x90,
  0x85, 0xf4, 0x88, 0xfe, 0xc7, 0x60, 0xe5, 0xfb, 0x02, 0x00
};

static const uint8_t webp_alpha_3x4_bgra[] = {
  0x00, 0x00, 0xff, 0xff, 0x00, 0xff, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00,
  0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x40, 0xff, 0x00, 0xff, 0xc0,
  0x30, 0x20, 0x10, 0xff, 0x60, 0x50, 0x40, 0x01, 0x90, 0x80, 0x70, 0xfe,
  0x00, 0x00, 0x00, 0x00, 0xf0, 0xe0, 0xd0, 0x7f, 0x38, 0x22, 0x0c, 0xc8
};

static const uint8_t webp_animation_offset_6x4[] = {
  0x52, 0x49, 0x46, 0x46, 0x84, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50,
  0x56, 0x50, 0x38, 0x58, 0x0a, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
  0x05, 0x00, 0x00, 0x03, 0x00, 0x00, 0x41, 0x4e, 0x49, 0x4d, 0x06, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x4e, 0x4d, 0x46,
  0x28, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
  0x00, 0x01, 0x00, 0x00, 0x64, 0x00, 0x00, 0x01, 0x56, 0x50, 0x38, 0x4c,
  0x0f, 0x00, 0x00, 0x00, 0x2f, 0x01, 0x40, 0x00, 0x00, 0x07, 0x10, 0xfd,
  0x8f, 0xfe, 0x07, 0x22, 0xa2, 0xff, 0x01, 0x00, 0x41, 0x4e, 0x4d, 0x46,
  0x28, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00,
  0x00, 0x01, 0x00, 0x00, 0x64, 0x00, 0x00, 0x01, 0x56, 0x50, 0x38, 0x4c,
  0x0f, 0x00, 0x00, 0x00, 0x2f, 0x01, 0x40, 0x00, 0x00, 0x07, 0xd0, 0xff,
  0x88, 0xfe, 0x07, 0x22, 0xa2, 0xff, 0x01, 0x00
};

static const uint8_t webp_animation_offset_6x4_bgra[] = {
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const uint8_t wave_file_data[] = {
  'R', 'I', 'F', 'F', 0x24, 0x00, 0x00, 0x00, 'W', 'A', 'V', 'E',
  'f', 'm', 't', ' ', 0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00,
  0x44, 0xac, 0x00, 0x00, 0x10, 0xb1, 0x02, 0x00, 0x04, 0x00, 0x10, 0x00,
  'd', 'a', 't', 'a', 0x00, 0x00, 0x00, 0x00
};

static const uint8_t avi_file_data[] = {
  'R', 'I', 'F', 'F', 0x24, 0x00, 0x00, 0x00, 'A', 'V', 'I', ' ',
  'L', 'I', 'S', 'T', 0x10, 0x00, 0x00, 0x00, 'h', 'd', 'r', 'l',
  'a', 'v', 'i', 'h', 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/*
 * Mock transport
 */
struct TestMailbox {
  ZZ9KMailboxDescriptor descriptor;
  ZZ9KMailboxWireEntry request_ring[MOCK_RING_ENTRIES];
  ZZ9KMailboxWireEntry completion_ring[MOCK_RING_ENTRIES];
};

static struct {
  struct TestMailbox mailbox;
  ZZ9KBoard board;
  void *board_window;
  uint32_t served_tail;
  uint32_t service_flags;
  const uint8_t *golden_pixels;
  uint32_t golden_size;
  uint32_t expected_width;
  uint32_t expected_height;
} g_mock;

void zz9k_set_idle_hook_for_test(void (*hook)(void));

static void *mock_window_alloc(uint32_t size)
{
#if defined(_WIN32)
  void *p = VirtualAlloc((LPVOID)MOCK_BOARD_ADDR, (SIZE_T)size,
                         MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (!p) {
    p = VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE | MEM_COMMIT,
                     PAGE_READWRITE);
  }
  return p;
#else
  void *p = mmap((void *)MOCK_BOARD_ADDR, (size_t)size,
                 PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  if (p == MAP_FAILED) {
    p = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  }
  return p == MAP_FAILED ? NULL : p;
#endif
}

static void mock_window_free(void *p, uint32_t size)
{
  if (!p) return;
#if defined(_WIN32)
  (void)size;
  VirtualFree(p, 0, MEM_RELEASE);
#else
  munmap(p, (size_t)size);
#endif
}

static ZZ9KMailboxWireEntry *mock_complete(const ZZ9KMailboxWireEntry *req,
                                           uint16_t status,
                                           uint16_t payload_len)
{
  struct TestMailbox *m = &g_mock.mailbox;
  ZZ9KMailboxWireEntry *reply;
  uint32_t tail = zz9k_get_be32(m->descriptor.completion_tail);

  reply = &m->completion_ring[tail % MOCK_RING_ENTRIES];
  memset((void *)reply->payload, 0, sizeof(reply->payload));
  zz9k_put_be32(reply->request_id, zz9k_get_be32(req->request_id));
  zz9k_put_be16(reply->opcode, zz9k_get_be16(req->opcode));
  zz9k_put_be16(reply->status, status);
  zz9k_put_be16(reply->payload_len, payload_len);
  zz9k_put_be32(reply->user_cookie, zz9k_get_be32(req->user_cookie));
  zz9k_put_be32(m->descriptor.completion_tail,
                (tail + 1U) % MOCK_RING_ENTRIES);
  zz9k_put_be32(m->descriptor.request_head,
                (zz9k_get_be32(m->descriptor.request_head) + 1U) %
                    MOCK_RING_ENTRIES);
  return reply;
}

static void mock_word(ZZ9KMailboxWireEntry *reply, uint32_t index,
                      uint32_t value)
{
  zz9k_put_be32(&reply->payload[index * 4U], value);
}

static uint32_t mock_req_word(const ZZ9KMailboxWireEntry *req,
                              uint32_t index)
{
  return zz9k_get_be32(&req->payload[index * 4U]);
}

static void mock_respond(void)
{
  struct TestMailbox *m = &g_mock.mailbox;
  const ZZ9KMailboxWireEntry *req;
  uint32_t head = zz9k_get_be32(m->descriptor.request_head);
  uint32_t tail = zz9k_get_be32(m->descriptor.request_tail);
  uint16_t opcode;
  ZZ9KMailboxWireEntry *reply;

  if (head == tail || g_mock.served_tail == tail)
    return;
  g_mock.served_tail = tail;
  req = &m->request_ring[head];
  opcode = zz9k_get_be16(req->opcode);

  switch (opcode) {
  case ZZ9K_OP_QUERY_CAPS:
    reply = mock_complete(req, ZZ9K_STATUS_OK, 40U);
    mock_word(reply, 0, ZZ9K_ABI_MAGIC);
    mock_word(reply, 1, ((uint32_t)ZZ9K_ABI_VERSION_MAJOR << 16) |
                            ZZ9K_ABI_VERSION_MINOR);
    mock_word(reply, 2, zz9k_get_be32(m->descriptor.capability_bits));
    mock_word(reply, 3, 48U);
    mock_word(reply, 4, 32U);
    mock_word(reply, 7, MOCK_RING_ENTRIES);
    mock_word(reply, 8, MOCK_RING_ENTRIES);
    break;

  case ZZ9K_OP_QUERY_SERVICE: {
    uint32_t service_id = mock_req_word(req, 0);
    reply = mock_complete(req, ZZ9K_STATUS_OK, sizeof(ZZ9KServiceInfoPayload));
    mock_word(reply, 0, service_id);
    mock_word(reply, 1, 1U); /* version */
    mock_word(reply, 2, 0U); /* capability_bits */
    mock_word(reply, 3, g_mock.service_flags); /* flags */
    mock_word(reply, 4, 0U); /* opcode_base */
    mock_word(reply, 5, 8U); /* opcode_count */
    mock_word(reply, 6, 32U); /* max_inline_payload */
    break;
  }

  case ZZ9K_OP_ALLOC_SHARED: {
    uint32_t length = mock_req_word(req, 0);
    uint32_t flags = mock_req_word(req, 2);
    reply = mock_complete(req, ZZ9K_STATUS_OK, 16U);
    mock_word(reply, 0, 0x40000001UL);
    mock_word(reply, 1, ZZ9K_ARM_MEMORY_START + 0x40000UL);
    mock_word(reply, 2, length);
    mock_word(reply, 3, flags);
    break;
  }

  case ZZ9K_OP_FREE_SHARED:
    (void)mock_complete(req, ZZ9K_STATUS_OK, 0U);
    break;

  case ZZ9K_OP_ALLOC_SURFACE: {
    uint32_t width = mock_req_word(req, 0);
    uint32_t height = mock_req_word(req, 1);
    uint32_t format = mock_req_word(req, 2);
    uint32_t flags = mock_req_word(req, 3);
    uint32_t pitch = mock_req_word(req, 4);
    reply = mock_complete(req, ZZ9K_STATUS_OK, 32U);
    mock_word(reply, 0, 0x50000001UL);
    mock_word(reply, 1, ZZ9K_ARM_MEMORY_START + 0x10000UL);
    mock_word(reply, 2, width);
    mock_word(reply, 3, height);
    mock_word(reply, 4, pitch);
    mock_word(reply, 5, format);
    mock_word(reply, 6, flags);
    mock_word(reply, 7, height * pitch);
    break;
  }

  case ZZ9K_OP_FREE_SURFACE:
    (void)mock_complete(req, ZZ9K_STATUS_OK, 0U);
    break;

  case ZZ9K_OP_IMAGE_SESSION_BEGIN:
    reply = mock_complete(req, ZZ9K_STATUS_OK,
                          sizeof(ZZ9KImageSessionResultPayload));
    mock_word(reply, 0, 1U);
    mock_word(reply, 1, ZZ9K_IMAGE_SESSION_STATE_NEED_INPUT);
    break;

  case ZZ9K_OP_IMAGE_SESSION_FEED: {
    uint32_t length = mock_req_word(req, 3);
    uint32_t flags = mock_req_word(req, 4);
    reply = mock_complete(req, ZZ9K_STATUS_OK,
                          sizeof(ZZ9KImageSessionResultPayload));
    mock_word(reply, 0, 1U);
    if ((flags & ZZ9K_IMAGE_SESSION_FEED_EOF) != 0U) {
      if (g_mock.golden_pixels && g_mock.golden_size > 0U && g_mock.board_window) {
        uint8_t *surf_ptr = (uint8_t *)g_mock.board_window +
                            (ZZ9K_AMIGA_MEMORY_OFFSET + 0x10000UL);
        memcpy(surf_ptr, g_mock.golden_pixels, g_mock.golden_size);
      }
      mock_word(reply, 1, ZZ9K_IMAGE_SESSION_STATE_COMPLETE);
      mock_word(reply, 2, g_mock.expected_width);
      mock_word(reply, 3, g_mock.expected_height);
      mock_word(reply, 4, ZZ9K_SURFACE_FORMAT_BGRA8888);
      mock_word(reply, 5, 0U);
      mock_word(reply, 6, 0U);
      mock_word(reply, 7, g_mock.expected_width);
      mock_word(reply, 8, g_mock.expected_height);
      mock_word(reply, 9, length);
      mock_word(reply, 10, g_mock.golden_size);
      mock_word(reply, 11, 0U);
    } else {
      mock_word(reply, 1, ZZ9K_IMAGE_SESSION_STATE_NEED_INPUT);
      mock_word(reply, 9, length);
    }
    break;
  }

  case ZZ9K_OP_IMAGE_SESSION_CLOSE:
    (void)mock_complete(req, ZZ9K_STATUS_OK, 0U);
    break;

  default:
    (void)mock_complete(req, ZZ9K_STATUS_UNSUPPORTED, 0U);
    break;
  }
}

static int mock_init(ZZ9KContext **out_ctx, uint32_t service_flags)
{
  uint32_t board_size = 0x01000000UL; /* 16 MiB */
  struct TestMailbox *m;
  void *window;

  memset(&g_mock, 0, sizeof(g_mock));
  window = mock_window_alloc(board_size);
  if (!window) {
    printf("mock_init: failed to allocate board window\n");
    return 0;
  }
  g_mock.board_window = window;
  g_mock.service_flags = service_flags;

  m = &g_mock.mailbox;
  zz9k_put_be32(m->descriptor.magic, ZZ9K_ABI_MAGIC);
  zz9k_put_be16(m->descriptor.abi_major, ZZ9K_ABI_VERSION_MAJOR);
  zz9k_put_be16(m->descriptor.abi_minor, ZZ9K_ABI_VERSION_MINOR);
  zz9k_put_be32(m->descriptor.descriptor_size,
                (uint32_t)sizeof(m->descriptor));
  zz9k_put_be32(m->descriptor.request_ring_offset,
                (uint32_t)offsetof(struct TestMailbox, request_ring));
  zz9k_put_be32(m->descriptor.request_ring_entries, MOCK_RING_ENTRIES);
  zz9k_put_be32(m->descriptor.completion_ring_offset,
                (uint32_t)offsetof(struct TestMailbox, completion_ring));
  zz9k_put_be32(m->descriptor.completion_ring_entries, MOCK_RING_ENTRIES);
  zz9k_put_be32(m->descriptor.capability_bits,
                ZZ9K_CAP_MAILBOX | ZZ9K_CAP_SERVICE_DISCOVERY |
                ZZ9K_CAP_IMAGE_DECODE | ZZ9K_CAP_SURFACES |
                ZZ9K_CAP_SHARED_ALLOC | ZZ9K_CAP_FRAMEBUFFER_SURFACE |
                ZZ9K_CAP_IMAGE_SCALE | ZZ9K_CAP_SURFACE_OPS);

  g_mock.board.zorro_version = 3U;
  g_mock.board.board_addr = (uint32_t)(uintptr_t)window;
  g_mock.board.board_size = board_size;
  g_mock.board.product = ZZ9K_PRODUCT_Z3;

  zz9k_set_idle_hook_for_test(mock_respond);

  if (zz9k_attach_mailbox(out_ctx, &g_mock.board, &m->descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    mock_window_free(window, board_size);
    zz9k_set_idle_hook_for_test(NULL);
    return 0;
  }
  return 1;
}

static void mock_cleanup(ZZ9KContext *ctx)
{
  if (ctx) {
    zz9k_close(ctx);
  }
  zz9k_set_idle_hook_for_test(NULL);
  if (g_mock.board_window) {
    mock_window_free(g_mock.board_window, 0x01000000UL);
    g_mock.board_window = NULL;
  }
}

static int write_temp_file(const char *path, const uint8_t *data, size_t size)
{
  FILE *f = fopen(path, "wb");
  if (!f) return 0;
  if (fwrite(data, 1U, size, f) != size) {
    fclose(f);
    return 0;
  }
  return fclose(f) == 0;
}

static int test_absent_image_webp_service_rejected(void)
{
  const char *path = "test_absent_flag.webp";
  ZZ9KContext *ctx = NULL;
  ZZ9KSurface fb;
  ZZ9KPictureViewerImage image;
  uint32_t flags_without_webp = ZZ9K_SERVICE_FLAG_IMAGE_STREAMING_INPUT |
                                ZZ9K_SERVICE_FLAG_IMAGE_PNG_DIRECT_BGRA;
  int rc;

  if (!write_temp_file(path, webp_lossy_3x5, sizeof(webp_lossy_3x5)))
    return 1;

  if (!mock_init(&ctx, flags_without_webp)) {
    remove(path);
    return 2;
  }

  memset(&fb, 0, sizeof(fb));
  fb.width = 640U;
  fb.height = 480U;
  fb.format = ZZ9K_SURFACE_FORMAT_BGRA8888;

  memset(&image, 0, sizeof(image));
  rc = zz9k_webp_decode_viewer_image(ctx, &fb, path, &image);
  mock_cleanup(ctx);
  remove(path);

  if (rc != 0 || image.surface_allocated != 0 ||
      image.codec != ZZ9K_PICTURE_VIEWER_CODEC_UNKNOWN) {
    printf("FAILED: absent IMAGE_WEBP flag was not rejected cleanly\n");
    return 3;
  }
  return 0;
}

static int test_wave_and_avi_rejected(void)
{
  const char *wave_path = "test_reject.wav";
  const char *avi_path = "test_reject.avi";
  ZZ9KContext *ctx = NULL;
  ZZ9KSurface fb;
  ZZ9KPictureViewerImage image;
  uint32_t flags = ZZ9K_SERVICE_FLAG_IMAGE_STREAMING_INPUT |
                   ZZ9K_SERVICE_FLAG_IMAGE_WEBP;
  int rc;

  if (!write_temp_file(wave_path, wave_file_data, sizeof(wave_file_data)) ||
      !write_temp_file(avi_path, avi_file_data, sizeof(avi_file_data))) {
    return 1;
  }

  if (!mock_init(&ctx, flags)) {
    remove(wave_path);
    remove(avi_path);
    return 2;
  }

  memset(&fb, 0, sizeof(fb));
  fb.width = 640U;
  fb.height = 480U;

  memset(&image, 0, sizeof(image));
  rc = zz9k_webp_decode_viewer_image(ctx, &fb, wave_path, &image);
  if (rc != 0 || image.surface_allocated != 0) {
    printf("FAILED: WAVE file was not rejected\n");
    mock_cleanup(ctx);
    remove(wave_path);
    remove(avi_path);
    return 3;
  }

  memset(&image, 0, sizeof(image));
  rc = zz9k_webp_decode_viewer_image(ctx, &fb, avi_path, &image);
  if (rc != 0 || image.surface_allocated != 0) {
    printf("FAILED: AVI file was not rejected\n");
    mock_cleanup(ctx);
    remove(wave_path);
    remove(avi_path);
    return 4;
  }

  mock_cleanup(ctx);
  remove(wave_path);
  remove(avi_path);
  return 0;
}

static int test_truncation_rejected(void)
{
  const char *trunc12_path = "test_trunc12.webp";
  const char *trunc20_path = "test_trunc20.webp";
  ZZ9KContext *ctx = NULL;
  ZZ9KSurface fb;
  ZZ9KPictureViewerImage image;
  uint32_t flags = ZZ9K_SERVICE_FLAG_IMAGE_STREAMING_INPUT |
                   ZZ9K_SERVICE_FLAG_IMAGE_WEBP;
  int rc;

  if (!write_temp_file(trunc12_path, webp_lossy_3x5, 12U) ||
      !write_temp_file(trunc20_path, webp_lossy_3x5, 20U)) {
    return 1;
  }

  if (!mock_init(&ctx, flags)) {
    remove(trunc12_path);
    remove(trunc20_path);
    return 2;
  }

  memset(&fb, 0, sizeof(fb));
  fb.width = 640U;
  fb.height = 480U;

  memset(&image, 0, sizeof(image));
  rc = zz9k_webp_decode_viewer_image(ctx, &fb, trunc12_path, &image);
  if (rc != 0 || image.surface_allocated != 0) {
    printf("FAILED: 12-byte truncated WebP was not rejected\n");
    mock_cleanup(ctx);
    remove(trunc12_path);
    remove(trunc20_path);
    return 3;
  }

  memset(&image, 0, sizeof(image));
  rc = zz9k_webp_decode_viewer_image(ctx, &fb, trunc20_path, &image);
  if (rc != 0 || image.surface_allocated != 0) {
    printf("FAILED: 20-byte truncated WebP was not rejected\n");
    mock_cleanup(ctx);
    remove(trunc12_path);
    remove(trunc20_path);
    return 4;
  }

  mock_cleanup(ctx);
  remove(trunc12_path);
  remove(trunc20_path);
  return 0;
}

static int test_failure_preserves_old_image(void)
{
  const char *bad_path = "test_bad.webp";
  ZZ9KContext *ctx = NULL;
  ZZ9KSurface fb;
  ZZ9KPictureViewerImage current_image;
  ZZ9KPictureViewerImage candidate;
  uint32_t flags = ZZ9K_SERVICE_FLAG_IMAGE_STREAMING_INPUT |
                   ZZ9K_SERVICE_FLAG_IMAGE_WEBP;
  int rc;

  if (!write_temp_file(bad_path, wave_file_data, sizeof(wave_file_data)))
    return 1;

  if (!mock_init(&ctx, flags)) {
    remove(bad_path);
    return 2;
  }

  /* Set up a pre-existing "current_image" */
  memset(&current_image, 0, sizeof(current_image));
  current_image.codec = ZZ9K_PICTURE_VIEWER_CODEC_JPEG;
  current_image.path = "previous_pic.jpg";
  current_image.width = 800U;
  current_image.height = 600U;
  current_image.surface.handle = 0x12345678UL;
  current_image.surface_allocated = 1;

  memset(&fb, 0, sizeof(fb));
  fb.width = 640U;
  fb.height = 480U;

  /* Simulating viewer navigation attempt on candidate */
  zz9k_picture_viewer_image_init(&candidate);
  rc = zz9k_webp_decode_viewer_image(ctx, &fb, bad_path, &candidate);
  if (rc == 0) {
    zz9k_picture_viewer_image_free(ctx, &candidate);
  }

  /* Current image must remain completely untouched */
  if (current_image.codec != ZZ9K_PICTURE_VIEWER_CODEC_JPEG ||
      current_image.width != 800U ||
      current_image.height != 600U ||
      current_image.surface.handle != 0x12345678UL ||
      current_image.surface_allocated != 1 ||
      strcmp(current_image.path, "previous_pic.jpg") != 0) {
    printf("FAILED: candidate failure altered existing image state\n");
    mock_cleanup(ctx);
    remove(bad_path);
    return 3;
  }

  mock_cleanup(ctx);
  remove(bad_path);
  return 0;
}

static int test_static_lossy_decode(void)
{
  const char *path = "test_lossy_3x5.webp";
  ZZ9KContext *ctx = NULL;
  ZZ9KSurface fb;
  ZZ9KPictureViewerImage image;
  uint32_t flags = ZZ9K_SERVICE_FLAG_IMAGE_STREAMING_INPUT |
                   ZZ9K_SERVICE_FLAG_IMAGE_WEBP;
  int rc;

  if (!write_temp_file(path, webp_lossy_3x5, sizeof(webp_lossy_3x5)))
    return 1;

  if (!mock_init(&ctx, flags)) {
    remove(path);
    return 2;
  }

  g_mock.golden_pixels = webp_lossy_3x5_bgra;
  g_mock.golden_size = sizeof(webp_lossy_3x5_bgra);
  g_mock.expected_width = 3U;
  g_mock.expected_height = 5U;

  memset(&fb, 0, sizeof(fb));
  fb.width = 640U;
  fb.height = 480U;

  memset(&image, 0, sizeof(image));
  rc = zz9k_webp_decode_viewer_image(ctx, &fb, path, &image);
  if (!rc) {
    printf("FAILED: static lossy WebP decode returned failure\n");
    mock_cleanup(ctx);
    remove(path);
    return 3;
  }

  if (image.codec != ZZ9K_PICTURE_VIEWER_CODEC_WEBP ||
      image.width != 3U || image.height != 5U ||
      !image.surface_allocated || !image.surface.data) {
    printf("FAILED: invalid image metadata after lossy WebP decode\n");
    zz9k_picture_viewer_image_free(ctx, &image);
    mock_cleanup(ctx);
    remove(path);
    return 4;
  }

  if (memcmp((const void *)image.surface.data, webp_lossy_3x5_bgra,
             sizeof(webp_lossy_3x5_bgra)) != 0) {
    printf("FAILED: decoded static lossy WebP pixels did not match reference\n");
    zz9k_picture_viewer_image_free(ctx, &image);
    mock_cleanup(ctx);
    remove(path);
    return 5;
  }

  zz9k_picture_viewer_image_free(ctx, &image);
  mock_cleanup(ctx);
  remove(path);
  return 0;
}

static int test_alpha_decode(void)
{
  const char *path = "test_alpha_3x4.webp";
  ZZ9KContext *ctx = NULL;
  ZZ9KSurface fb;
  ZZ9KPictureViewerImage image;
  uint32_t flags = ZZ9K_SERVICE_FLAG_IMAGE_STREAMING_INPUT |
                   ZZ9K_SERVICE_FLAG_IMAGE_WEBP;
  int rc;

  if (!write_temp_file(path, webp_alpha_3x4, sizeof(webp_alpha_3x4)))
    return 1;

  if (!mock_init(&ctx, flags)) {
    remove(path);
    return 2;
  }

  g_mock.golden_pixels = webp_alpha_3x4_bgra;
  g_mock.golden_size = sizeof(webp_alpha_3x4_bgra);
  g_mock.expected_width = 3U;
  g_mock.expected_height = 4U;

  memset(&fb, 0, sizeof(fb));
  fb.width = 640U;
  fb.height = 480U;

  memset(&image, 0, sizeof(image));
  rc = zz9k_webp_decode_viewer_image(ctx, &fb, path, &image);
  if (!rc) {
    printf("FAILED: alpha WebP decode returned failure\n");
    mock_cleanup(ctx);
    remove(path);
    return 3;
  }

  if (image.codec != ZZ9K_PICTURE_VIEWER_CODEC_WEBP ||
      image.width != 3U || image.height != 4U ||
      !image.surface_allocated || !image.surface.data) {
    printf("FAILED: invalid image metadata after alpha WebP decode\n");
    zz9k_picture_viewer_image_free(ctx, &image);
    mock_cleanup(ctx);
    remove(path);
    return 4;
  }

  if (memcmp((const void *)image.surface.data, webp_alpha_3x4_bgra,
             sizeof(webp_alpha_3x4_bgra)) != 0) {
    printf("FAILED: decoded alpha WebP pixels did not match reference\n");
    zz9k_picture_viewer_image_free(ctx, &image);
    mock_cleanup(ctx);
    remove(path);
    return 5;
  }

  zz9k_picture_viewer_image_free(ctx, &image);
  mock_cleanup(ctx);
  remove(path);
  return 0;
}

static int test_animation_preview_decode(void)
{
  const char *path = "test_anim_6x4.webp";
  ZZ9KContext *ctx = NULL;
  ZZ9KSurface fb;
  ZZ9KPictureViewerImage image;
  uint32_t flags = ZZ9K_SERVICE_FLAG_IMAGE_STREAMING_INPUT |
                   ZZ9K_SERVICE_FLAG_IMAGE_WEBP;
  int rc;

  if (!write_temp_file(path, webp_animation_offset_6x4,
                       sizeof(webp_animation_offset_6x4)))
    return 1;

  if (!mock_init(&ctx, flags)) {
    remove(path);
    return 2;
  }

  g_mock.golden_pixels = webp_animation_offset_6x4_bgra;
  g_mock.golden_size = sizeof(webp_animation_offset_6x4_bgra);
  g_mock.expected_width = 6U;
  g_mock.expected_height = 4U;

  memset(&fb, 0, sizeof(fb));
  fb.width = 640U;
  fb.height = 480U;

  memset(&image, 0, sizeof(image));
  rc = zz9k_webp_decode_viewer_image(ctx, &fb, path, &image);
  if (!rc) {
    printf("FAILED: animation preview WebP decode returned failure\n");
    mock_cleanup(ctx);
    remove(path);
    return 3;
  }

  if (image.codec != ZZ9K_PICTURE_VIEWER_CODEC_WEBP ||
      image.width != 6U || image.height != 4U ||
      !image.surface_allocated || !image.surface.data) {
    printf("FAILED: invalid image metadata after animation preview decode\n");
    zz9k_picture_viewer_image_free(ctx, &image);
    mock_cleanup(ctx);
    remove(path);
    return 4;
  }

  if (memcmp((const void *)image.surface.data, webp_animation_offset_6x4_bgra,
             sizeof(webp_animation_offset_6x4_bgra)) != 0) {
    printf("FAILED: decoded animated preview WebP pixels did not match reference canvas\n");
    zz9k_picture_viewer_image_free(ctx, &image);
    mock_cleanup(ctx);
    remove(path);
    return 5;
  }

  zz9k_picture_viewer_image_free(ctx, &image);
  mock_cleanup(ctx);
  remove(path);
  return 0;
}

static int test_no_shell_out(const char *source_path)
{
  FILE *f;
  long len;
  char *buf;
  int ok = 1;

  if (!source_path)
    return 0;

  f = fopen(source_path, "rb");
  if (!f) {
    printf("could not open source file %s\n", source_path);
    return 1;
  }
  fseek(f, 0, SEEK_END);
  len = ftell(f);
  fseek(f, 0, SEEK_SET);
  buf = (char *)malloc((size_t)len + 1U);
  if (!buf) {
    fclose(f);
    return 1;
  }
  if (fread(buf, 1U, (size_t)len, f) != (size_t)len) {
    free(buf);
    fclose(f);
    return 1;
  }
  buf[len] = '\0';
  fclose(f);

  if (strstr(buf, "system(") != NULL) {
    printf("FAILED: source contains system()\n");
    ok = 0;
  }
  if (strstr(buf, "exec(") != NULL || strstr(buf, "execl(") != NULL ||
      strstr(buf, "execv(") != NULL) {
    printf("FAILED: source contains exec\n");
    ok = 0;
  }
  if (strstr(buf, "popen(") != NULL) {
    printf("FAILED: source contains popen()\n");
    ok = 0;
  }
  if (strstr(buf, "zz9k_webp_decode_viewer_image") == NULL) {
    printf("FAILED: source missing zz9k_webp_decode_viewer_image\n");
    ok = 0;
  }

  free(buf);
  return ok ? 0 : 2;
}

int main(int argc, char **argv)
{
  int err;

  if (argc > 1) {
    err = test_no_shell_out(argv[1]);
    if (err) return err + 70;
  }

  err = test_absent_image_webp_service_rejected();
  if (err) return err;

  err = test_wave_and_avi_rejected();
  if (err) return err + 10;

  err = test_truncation_rejected();
  if (err) return err + 20;

  err = test_failure_preserves_old_image();
  if (err) return err + 30;

  err = test_static_lossy_decode();
  if (err) return err + 40;

  err = test_alpha_decode();
  if (err) return err + 50;

  err = test_animation_preview_decode();
  if (err) return err + 60;

  printf("All zz9k-view WebP bridge behavior tests passed.\n");
  return 0;
}
