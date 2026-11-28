/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>

#include "aar/app.h"
#include "aar/ext_zz9000.h"
#include "zzapp_kernel.h"
#include "zzapp_rec.h"
#include "zzapp_runtime.h"
#include "zzapp_svc.h"
#include "xil_cache.h"

struct xil_cache_mock_state g_xil_cache_mock;
int smp_cpu_id(void) { return 0; }
uint32_t smp_local_irq_save(void) { return 0U; }
void smp_local_irq_restore(uint32_t state) { (void)state; }
void smp_raw_spin_lock(volatile uint32_t *word) { *word = 1U; }
int smp_raw_spin_try_lock(volatile uint32_t *word)
{
    if (*word)
        return 0;
    *word = 1U;
    return 1;
}
void smp_raw_spin_unlock(volatile uint32_t *word) { *word = 0U; }

#define WINDOW 0xc0000000U
#define SECTION 0x15de6U

static uint32_t bsp[4096];
static int failures;
static unsigned checks;


#define CHECK(expr) do { \
    checks++; \
    if (!(expr)) { \
        failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    } \
} while (0)

static uint32_t user_va;

/*
 * The records the SVC layer keeps its state in: eight of them in a static
 * arena below 4 GiB (table addresses go into 32-bit descriptors), with the
 * scheduler attached so that zzapp_svc_counter_block follows the thread.
 */
#define SVC_TEST_ARENA_BYTES (2U << 20)
static uint8_t arena_mem[SVC_TEST_ARENA_BYTES] __attribute__((aligned(1U << 20)));
static zzapp_kernel_t kernel;

static void setup_records(void)
{
    zzapp_rec_layout_t layout;

    zzapp_rec_layout_for(8U, 4U << 20, &layout);
    CHECK(layout.bytes <= SVC_TEST_ARENA_BYTES);
    CHECK(zzapp_rec_arena_init(arena_mem, &layout) == 0);
    CHECK(zzapp_kernel_init(&kernel, zzapp_rec_by_pid(0U),
                            ZZAPP_REC_RECORD_BYTES, zzapp_rec_capacity()) == 0);
    zzapp_kernel_set_current_thread(0);
}

static void setup(zzapp_as_t *as)
{
    setup_records();
    memset(bsp, 0, sizeof(bsp));
    bsp[0] = SECTION;
    bsp[0x20U] = SECTION;
    zzapp_as_init(bsp);
    CHECK(zzapp_as_create(as, 0U) == 0);
    CHECK(zzapp_as_map_image(as, WINDOW, 0x01000000U, 4096U,
                             ZZAPP_AS_IMAGE_RW) == 0);
    CHECK(zzapp_as_map_image(as, WINDOW + 0x1000U, 0x01001000U, 4096U,
                             ZZAPP_AS_IMAGE_RO) == 0);
    CHECK(zzapp_as_alias_map(as, 0x02000000U, 4096U, 1, &user_va) == 0);
}
static zzapp_proc_t proc;
static aar_api api;
static aar_gl gl;
static uint8_t *user_page;
static uint8_t *heap_page;
static unsigned vertex_pointer_calls;
static unsigned draw_arrays_calls;
static unsigned draw_elements_calls;
static unsigned texture_upload_calls;
static unsigned texture_sub_upload_calls;
static unsigned read_pixels_calls;
static unsigned build_mipmaps_calls;
static unsigned getter_calls;
static uintptr_t getter_pointer;
static unsigned gl_begin_calls;
static unsigned gl_end_calls;
static unsigned msg_poll_calls;
static unsigned file_seek_calls;
static unsigned heap_range_calls;
static unsigned file_list_next_calls;
static unsigned heap_shrink_calls;
static unsigned should_stop_calls;
static unsigned time_us_calls;
static uint32_t vertex2f_words[2];
static unsigned lock_arrays_calls;
static unsigned unlock_arrays_calls;
static uint8_t mock_vertex_array_enabled;
static uint8_t mock_array_transformations_enabled;
static uint32_t mock_gl_error;
#if ZZAPP_PERF_COUNTERS
static uint32_t svc_counter_now;

uint32_t zzapp_svc_test_pmccntr(void)
{
    svc_counter_now += 10U;
    return svc_counter_now;
}

void zzapp_svc_counters_test_eret(uint32_t pid, uint32_t stamp);
void zzapp_svc_counters_test_snapshot(uint32_t pid, uint32_t *total_calls,
                                      uint32_t samples[4], uint32_t averages[4]);
#endif

/* The counters live in the record: the vector stores through the pointer the
 * scheduler published for the running thread. */
#if ZZAPP_PERF_COUNTERS
static void asm_entry_stamp(uint32_t stamp)
{
    *(volatile uint32_t *)zzapp_svc_counter_block = stamp;
}
#endif

static zzapp_proc_t extra_procs[8];
zzapp_proc_t *zzapp_proc_slot(uint32_t pid)
{
    if (pid == 0U)
        return &proc;
    return pid < 8U ? &extra_procs[pid] : NULL;
}
const aar_api *zzapp_api_table(void) { return &api; }
static uint32_t services_pid;
uint32_t zzapp_services_current_pid(void) { return services_pid; }

static const aar_gl *mock_gl_bind(uint32_t surface_id)
{
    return surface_id ? &gl : NULL;
}

static const char *mock_renderer;

static const uint8_t *mock_get_string(uint32_t name)
{
    switch (name) {
    case 0x1f00U: return (const uint8_t *)"test vendor";
    case 0x1f01U: return (const uint8_t *)mock_renderer;
    case 0x1f02U: return (const uint8_t *)"1.1";
    case 0x1f03U: return (const uint8_t *)"";
    default: return NULL;
    }
}

static void mock_vertex2f(float x, float y)
{
    memcpy(&vertex2f_words[0], &x, sizeof(x));
    memcpy(&vertex2f_words[1], &y, sizeof(y));
}

static void mock_vertex_pointer(int32_t size, uint32_t type, int32_t stride,
                                const void *pointer)
{
    (void)size;
    (void)type;
    (void)stride;
    (void)pointer;
    ++vertex_pointer_calls;
}
static void mock_enable_client_state(uint32_t state) { (void)state; }
static void mock_disable_client_state(uint32_t state) { (void)state; }
static uint8_t mock_is_enabled(uint32_t cap)
{
    if (cap == 0x8074U)
        return mock_vertex_array_enabled;
    if (cap == 0x701dU)
        return mock_array_transformations_enabled;
    return 0U;
}
static void mock_lock_arrays(uint32_t first, int32_t count)
{
    (void)first;
    (void)count;
    ++lock_arrays_calls;
}
static void mock_unlock_arrays(void)
{
    ++unlock_arrays_calls;
}
static void mock_pixel_store(uint32_t pname, int32_t value)
{
    (void)pname;
    (void)value;
}
static void mock_draw_arrays(uint32_t mode, int32_t first, int32_t count)
{
    (void)mode;
    (void)first;
    (void)count;
    ++draw_arrays_calls;
}
static void mock_draw_elements(uint32_t mode, int32_t count, uint32_t type,
                               const void *indices)
{
    (void)mode;
    (void)count;
    (void)type;
    (void)indices;
    ++draw_elements_calls;
}
static void mock_client_active_texture(uint32_t texture)
{
    (void)texture;
}
static void mock_gl_unbind(void) {}
typedef struct {
    uint32_t pointer;
    int32_t size;
    uint32_t type;
    int32_t stride;
    uint8_t enabled;
} reference_array_t;

static uint32_t reference_type_bytes(uint32_t type)
{
    switch (type) {
    case 0x1400U: case 0x1401U: return 1U;
    case 0x1402U: case 0x1403U: return 2U;
    case 0x1404U: case 0x1405U: case 0x1406U: return 4U;
    default: return 0U;
    }
}
static int reference_array_accepts(const zzapp_as_t *as,
                                   const reference_array_t *array,
                                   uint32_t index)
{
    uint32_t type_bytes;
    uint32_t element_bytes;
    uint64_t address;
    uint32_t step;

    if (!array->enabled) return 1;
    type_bytes = reference_type_bytes(array->type);
    if (array->size <= 0 || array->size > 4 || !type_bytes ||
        array->stride < 0)
        return 1;
    element_bytes = (uint32_t)array->size * type_bytes;
    if (!array->pointer) return 0;
    step = array->stride ? (uint32_t)array->stride : element_bytes;
    address = (uint64_t)array->pointer + (uint64_t)index * step;
    return address <= UINT32_MAX &&
           zzapp_svc_check_user(as, (uint32_t)address, element_bytes, 0) == AAR_OK;
}
static int reference_draw_accepts(const zzapp_as_t *as,
                                  const reference_array_t arrays[3],
                                  const uint32_t *indices, uint32_t count)
{
    uint32_t i;
    uint32_t array;

    for (i = 0U; i < count; ++i)
        for (array = 0U; array < 3U; ++array)
            if (!reference_array_accepts(as, &arrays[array], indices[i]))
                return 0;
    return 1;
}

static void mock_texture_upload(uint32_t target, int32_t level,
                                int32_t internal_format, int32_t width,
                                int32_t height, int32_t border,
                                uint32_t format, uint32_t type,
                                const void *pixels)
{
    (void)target;
    (void)level;
    (void)internal_format;
    (void)width;
    (void)height;
    (void)border;
    (void)format;
    (void)type;
    (void)pixels;
    ++texture_upload_calls;
}
static void mock_texture_sub_upload(uint32_t target, int32_t level,
                                    int32_t xoffset, int32_t yoffset,
                                    int32_t width, int32_t height,
                                    uint32_t format, uint32_t type,
                                    const void *pixels)
{
    (void)target;
    (void)level;
    (void)xoffset;
    (void)yoffset;
    (void)width;
    (void)height;
    (void)format;
    (void)type;
    (void)pixels;
    ++texture_sub_upload_calls;
}
static void mock_read_pixels(int32_t x, int32_t y, int32_t width,
                             int32_t height, uint32_t format, uint32_t type,
                             void *pixels)
{
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)format;
    (void)type;
    (void)pixels;
    ++read_pixels_calls;
}
static int32_t mock_build_mipmaps(uint32_t target, int32_t internal_format,
                                  int32_t width, int32_t height,
                                  uint32_t format, uint32_t type,
                                  const void *pixels)
{
    (void)target;
    (void)internal_format;
    (void)width;
    (void)height;
    (void)format;
    (void)type;
    (void)pixels;
    ++build_mipmaps_calls;
    return 0;
}
static int mock_file_list_next(int list, aar_dirent *entry)
{
    (void)list;
    ++file_list_next_calls;
    if (file_list_next_calls == 1U) {
        memset(entry, 0x31, sizeof(*entry));
        return AAR_OK;
    }
    return AAR_E_INVAL;
}
static int mock_heap_shrink(uint64_t bytes, void **new_break)
{
    (void)bytes;
    (void)new_break;
    ++heap_shrink_calls;
    return AAR_OK;
}
static uint32_t mock_get_error(void)
{
    uint32_t error = mock_gl_error;

    mock_gl_error = 0U;
    return error;
}
static void mock_begin(uint32_t mode)
{
    if (mode > 9U)
        mock_gl_error = 0x0500U;
    ++gl_begin_calls;
}
static void mock_end(void)
{
    ++gl_end_calls;
}
static void mock_get_integerv(uint32_t pname, int32_t *params)
{
    (void)pname;
    ++getter_calls;
    getter_pointer = (uintptr_t)params;
    (void)params;
}
static int mock_msg_poll(void *message)
{
    (void)message;
    ++msg_poll_calls;
    return 1;
}
/*
 * Handlers that take a 64-bit value are written as the dispatcher calls them:
 * twelve uint32_t words, exactly what an A32 callee finds in r0-r3 and on the
 * stack, a uint64_t in an even register pair or an 8-aligned stack pair. On
 * this 64-bit host those words are not the C signature's, so the mock takes
 * the words and is entered through a cast. Pointers it receives are the
 * dispatcher's 32-bit-truncated kernel copies and must never be dereferenced.
 */
static int64_t file_seek_offset_seen;
static int file_seek_whence_seen;
static int mock_file_seek_words(uint32_t w0, uint32_t w1, uint32_t lo,
                                uint32_t hi, uint32_t whence, uint32_t w5,
                                uint32_t w6, uint32_t w7, uint32_t w8,
                                uint32_t w9, uint32_t w10, uint32_t w11)
{
    (void)w0; (void)w1; (void)w5; (void)w6; (void)w7;
    (void)w8; (void)w9; (void)w10; (void)w11;
    file_seek_offset_seen = (int64_t)(((uint64_t)hi << 32) | lo);
    file_seek_whence_seen = (int)whence;
    ++file_seek_calls;
    return AAR_OK;
}
static void mock_heap_range(void **base, uint64_t *bytes)
{
    ++heap_range_calls;
    if (base)
        *base = NULL;
    if (bytes)
        *bytes = 0U;
}

static void mock_exit(int code)
{
    (void)code;
}

static int mock_should_stop(void)
{
    ++should_stop_calls;
    return 1;
}

static uint64_t mock_time_us(void)
{
    ++time_us_calls;
    return UINT64_C(0x1122334455667788);
}

static aar_ext_zz9000 runtime_ext;
static uint32_t ext_bind_flags_seen;
static unsigned sha256_calls;
static uint64_t sha256_bytes_seen;
static unsigned decompress_calls;
static uint32_t decompress_algorithm_seen;
static uint32_t decompress_flags_seen;
static uint64_t decompress_src_bytes_seen;
static uint64_t decompress_dst_bytes_seen;
static const void *decompress_src_seen;
static void *decompress_dst_seen;
static unsigned info_calls;

static const aar_gl *mock_ext_bind_flags(uint32_t surface_id, uint32_t flags)
{
    ext_bind_flags_seen = flags;
    return surface_id ? &gl : NULL;
}

static const void *mock_ext(uint32_t platform_id, uint32_t ext_id)
{
    if (platform_id != AAR_PLATFORM_ZZ9000 || ext_id != AAR_EXT_ZZ9000_MAIN)
        return NULL;
    return &runtime_ext;
}

static int mock_sha256_words(uint32_t w0, uint32_t w1, uint32_t lo,
                             uint32_t hi, uint32_t w4, uint32_t w5,
                             uint32_t w6, uint32_t w7, uint32_t w8,
                             uint32_t w9, uint32_t w10, uint32_t w11)
{
    (void)w0; (void)w1; (void)w4; (void)w5; (void)w6;
    (void)w7; (void)w8; (void)w9; (void)w10; (void)w11;
    ++sha256_calls;
    sha256_bytes_seen = ((uint64_t)hi << 32) | lo;
    return AAR_OK;
}

static int mock_decompress_words(uint32_t algorithm, uint32_t flags,
                                 uint32_t src, uint32_t w3, uint32_t src_lo,
                                 uint32_t src_hi, uint32_t dst, uint32_t w7,
                                 uint32_t dst_lo, uint32_t dst_hi,
                                 uint32_t w10, uint32_t w11)
{
    (void)w3; (void)w7; (void)w10; (void)w11;
    ++decompress_calls;
    decompress_algorithm_seen = algorithm;
    decompress_flags_seen = flags;
    decompress_src_seen = (const void *)(uintptr_t)src;
    decompress_src_bytes_seen = ((uint64_t)src_hi << 32) | src_lo;
    decompress_dst_seen = (void *)(uintptr_t)dst;
    decompress_dst_bytes_seen = ((uint64_t)dst_hi << 32) | dst_lo;
    return AAR_OK;
}

static int mock_info(aar_info *out)
{
    (void)out;
    ++info_calls;
    return AAR_OK;
}

static const zzapp_svc_descriptor_t *api_descriptor(size_t field)
{
    return zzapp_svc_api_descriptor((uint32_t)((field - offsetof(aar_api, exit)) /
                                               sizeof(void *)));
}

static uint32_t gl_slot(size_t field)
{
    return (uint32_t)((field - offsetof(aar_gl, Begin)) / sizeof(void *));
}
static uint32_t api_slot(size_t field)
{
    return (uint32_t)((field - offsetof(aar_api, exit)) / sizeof(void *));
}
static void dispatch_words(uint32_t number, zzapp_svc_frame_t *frame,
                           const uint32_t *args, uint32_t words)
{
    uint32_t i;

    memset(frame, 0, sizeof(*frame));
    *(uint32_t *)(void *)user_page = 0xef000000U;
    frame->exception_lr = user_va + 4U;
    frame->r[12] = number;
    for (i = 0U; i < 4U && i < words; ++i)
        frame->r[i] = args[i];
    frame->user_sp = user_va + 0x800U;
    for (; i < words; ++i)
        *(uint32_t *)(void *)(user_page + 0x800U + (i - 4U) * 4U) = args[i];
    zzapp_svc_dispatch(frame);
}
static void dispatch(uint32_t number, zzapp_svc_frame_t *frame,
                     const uint32_t args[9])
{
    dispatch_words(number, frame, args, 9U);
}
static void setup_dispatch(void)
{
    void *mapped;

    memset(&proc, 0, sizeof(proc));
    setup(&proc.as);
    mapped = mmap((void *)(uintptr_t)user_va, 8192U,
                  PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS |
                  MAP_FIXED, -1, 0);
    CHECK(mapped != MAP_FAILED);
    user_page = mapped == MAP_FAILED ? NULL : mapped;
    mapped = mmap((void *)(uintptr_t)WINDOW, 4096U,
                  PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS |
                  MAP_FIXED, -1, 0);
    CHECK(mapped != MAP_FAILED);
    heap_page = mapped == MAP_FAILED ? NULL : mapped;
    memset(&api, 0, sizeof(api));
    memset(&gl, 0, sizeof(gl));
    api.size = sizeof(api);
    api.gl_bind = mock_gl_bind;
    gl.size = sizeof(gl);
    api.file_seek = (int (*)(int, int64_t, int, int64_t *))(void *)
                    mock_file_seek_words;
    api.should_stop = mock_should_stop;
    api.time_us = mock_time_us;
    api.heap_range = mock_heap_range;
    api.msg_poll = mock_msg_poll;
    api.sha256 = (int (*)(const void *, uint64_t, uint8_t *))(void *)
                 mock_sha256_words;
    api.decompress = (int (*)(uint32_t, uint32_t, const void *, uint64_t,
                              void *, uint64_t, aar_service_result *))(void *)
                     mock_decompress_words;
    api.info = mock_info;
    api.ext = mock_ext;
    memset(&runtime_ext, 0, sizeof(runtime_ext));
    runtime_ext.size = sizeof(runtime_ext);
    runtime_ext.gl_bind_flags = mock_ext_bind_flags;
    runtime_ext.build_id = "fw-build-0123";
    runtime_ext.gl_build_id = "must-not-reach-the-app";
    ext_bind_flags_seen = 0U;
    sha256_calls = 0U;
    decompress_calls = 0U;
    info_calls = 0U;
    gl.Begin = mock_begin;
    gl.End = mock_end;
    gl.GetIntegerv = mock_get_integerv;
    gl.GetFloatv = (void (*)(uint32_t, float *))mock_get_integerv;
    gl.Vertex2f = mock_vertex2f;
    gl.VertexPointer = mock_vertex_pointer;
    gl.EnableClientState = mock_enable_client_state;
    gl.IsEnabled = mock_is_enabled;
    gl.DisableClientState = mock_disable_client_state;
    gl.DrawArrays = mock_draw_arrays;
    gl.ColorPointer = mock_vertex_pointer;
    gl.TexCoordPointer = mock_vertex_pointer;
    gl.ClientActiveTextureARB = mock_client_active_texture;
    gl.LockArrays = mock_lock_arrays;
    gl.UnlockArrays = mock_unlock_arrays;
    api.gl_unbind = mock_gl_unbind;
    draw_elements_calls = 0U;


    gl.DrawElements = mock_draw_elements;
    gl.TexImage2D = mock_texture_upload;
    gl.PixelStorei = mock_pixel_store;
    gl.ReadPixels = mock_read_pixels;
    gl.TexSubImage2D = mock_texture_sub_upload;
    gl.Build2DMipmaps = mock_build_mipmaps;
    api.file_list_next = mock_file_list_next;
    api.heap_shrink = mock_heap_shrink;
    gl.GetError = mock_get_error;
    vertex_pointer_calls = 0U;
    draw_arrays_calls = 0U;
    texture_upload_calls = 0U;
    texture_sub_upload_calls = 0U;
    read_pixels_calls = 0U;
    build_mipmaps_calls = 0U;
    getter_calls = 0U;
    getter_pointer = 0U;
    gl_begin_calls = 0U;
    gl_end_calls = 0U;
    msg_poll_calls = 0U;
    file_seek_calls = 0U;
    heap_range_calls = 0U;
    should_stop_calls = 0U;
    time_us_calls = 0U;
    vertex2f_words[0] = 0U;
    vertex2f_words[1] = 0U;
    lock_arrays_calls = 0U;
    unlock_arrays_calls = 0U;
    mock_vertex_array_enabled = 0U;
    mock_array_transformations_enabled = 1U;
    mock_gl_error = 0U;
}
static void teardown_dispatch(void)
{
    if (heap_page)
        munmap(heap_page, 4096U);
    heap_page = NULL;
    if (user_page)
        munmap(user_page, 8192U);
    user_page = NULL;
    zzapp_as_destroy(&proc.as);
}

static unsigned file_request_calls;

static int mock_file_request(uint32_t flags, const char *title,
                             const char *initial, char *path, uint32_t capacity)
{
    file_request_calls++;
    CHECK(flags == AAR_FILE_REQUEST_SAVE && title == NULL && initial == NULL);
    CHECK(capacity == 2U);
    if (path == (char *)user_page + 0x100U && capacity == 2U)
        memcpy(path, "X", 2U);
    return 1;
}

static void test_file_request_pointers(void)
{
    zzapp_svc_frame_t frame;
    uint32_t slot = api_slot(offsetof(aar_api, file_request));
    uint32_t args[5] = { AAR_FILE_REQUEST_SAVE, 0U, 0U, 0U, 2U };

    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    api.file_request = mock_file_request;
    file_request_calls = 0U;
    args[3] = user_va + 0x100U;
    args[1] = user_va + 0x1000U;
    dispatch_words(slot, &frame, args, 5U);
    CHECK((int32_t)frame.r[0] == AAR_E_FAULT && file_request_calls == 0U);
    args[2] = args[1];
    args[1] = 0U;
    dispatch_words(slot, &frame, args, 5U);
    CHECK((int32_t)frame.r[0] == AAR_E_FAULT && file_request_calls == 0U);
    memset(user_page + 0x200U, 'a', 512U);
    user_page[0x400U] = '\0';
    args[2] = user_va + 0x200U;
    dispatch_words(slot, &frame, args, 5U);
    CHECK((int32_t)frame.r[0] == AAR_E_INVAL && file_request_calls == 0U);
    args[1] = args[2];
    args[2] = 0U;
    dispatch_words(slot, &frame, args, 5U);
    CHECK((int32_t)frame.r[0] == AAR_E_INVAL && file_request_calls == 0U);
    args[1] = 0U;
    args[3] = WINDOW + 0x1000U;
    dispatch_words(slot, &frame, args, 5U);
    CHECK((int32_t)frame.r[0] == AAR_E_FAULT && file_request_calls == 0U);
    args[3] = user_va + 0xfffU;
    dispatch_words(slot, &frame, args, 5U);
    CHECK((int32_t)frame.r[0] == AAR_E_FAULT && file_request_calls == 0U);
    args[3] = user_va + 0x100U;
    dispatch_words(slot, &frame, args, 5U);
    CHECK(frame.r[0] == 1U && file_request_calls == 1U);
    CHECK(memcmp(user_page + 0x100U, "X", 2U) == 0);
    teardown_dispatch();
}

static unsigned requester_calls;
static int requester_result;

static int mock_requester(const char *title, const char *text,
                          const char *buttons, uint32_t *choice)
{
    requester_calls++;
    CHECK(title == NULL);
    CHECK(text && strcmp(text, "100% done") == 0);
    CHECK(buttons && strcmp(buttons, "OK|Retry") == 0);
    CHECK(choice && choice != (uint32_t *)(void *)(user_page + 0x100U));
    if (choice)
        *choice = 2U;
    return requester_result;
}

static void test_requester_pointers(void)
{
    zzapp_svc_frame_t frame;
    uint32_t slot = api_slot(offsetof(aar_api, requester));
    uint32_t args[4], good[4], i;
    const uint32_t max_bytes[] = { AAR_REQUESTER_TITLE_MAX + 1U,
        AAR_REQUESTER_TEXT_MAX + 1U, AAR_REQUESTER_BUTTONS_MAX + 1U };
    uint32_t *choice;

    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    api.requester = mock_requester;
    requester_calls = 0U;
    good[0] = 0U;
    good[1] = user_va + 0x110U;
    good[2] = user_va + 0x120U;
    good[3] = user_va + 0x100U;
    memcpy(user_page + 0x110U, "100% done", sizeof("100% done"));
    memcpy(user_page + 0x120U, "OK|Retry", sizeof("OK|Retry"));
    choice = (uint32_t *)(void *)(user_page + 0x100U);
    *choice = 99U;

    /* Each string starts on a readable page but runs into an unmapped page. */
    user_page[0xfffU] = 'x';
    for (i = 0U; i < 3U; i++) {
        memcpy(args, good, sizeof(args));
        args[i] = user_va + 0xfffU;
        dispatch_words(slot, &frame, args, 4U);
        CHECK((int32_t)frame.r[0] == AAR_E_FAULT && requester_calls == 0U);

        memset(user_page + 0x200U, 'x', max_bytes[i]);
        user_page[0x200U + max_bytes[i]] = '\0';
        args[i] = user_va + 0x200U;
        dispatch_words(slot, &frame, args, 4U);
        CHECK((int32_t)frame.r[0] == AAR_E_INVAL && requester_calls == 0U);
    }
    memcpy(args, good, sizeof(args));
    args[3] = WINDOW + 0x1000U;
    dispatch_words(slot, &frame, args, 4U);
    CHECK((int32_t)frame.r[0] == AAR_E_FAULT && requester_calls == 0U);
    args[3] = user_va + 0xffeU;
    dispatch_words(slot, &frame, args, 4U);
    CHECK((int32_t)frame.r[0] == AAR_E_FAULT && requester_calls == 0U);
    CHECK(*choice == 99U);

    requester_result = AAR_E_NOSYS;
    dispatch_words(slot, &frame, good, 4U);
    CHECK((int32_t)frame.r[0] == AAR_E_NOSYS && requester_calls == 1U && *choice == 99U);
    requester_result = AAR_OK;
    dispatch_words(slot, &frame, good, 4U);
    CHECK((int32_t)frame.r[0] == AAR_OK && requester_calls == 2U && *choice == 2U);
    teardown_dispatch();
}

static void test_table_copy_uses_only_veneers(void)
{
    aar_api direct;
    aar_api app;
    uintptr_t *source;
    uintptr_t *entries;
    uint32_t i;

    memset(&direct, 0, sizeof(direct));
    direct.size = sizeof(direct);
    direct.version = AAR_API_VERSION;
    direct.groups = AAR_GROUPS_DEFINED;
    direct.platform_id = AAR_PLATFORM_ZZ9000;
    source = (uintptr_t *)(void *)((uint8_t *)&direct + offsetof(aar_api, exit));
    for (i = 0; i < zzapp_svc_api_count(); ++i)
        source[i] = 0x12345678U;
    /* audio_stats is optional: a NULL slot stays NULL for the app. */
    direct.audio_stats = NULL;
    CHECK(zzapp_svc_copy_api(&app, &direct) == AAR_OK);
    CHECK(app.size == direct.size);
    CHECK(app.version == direct.version);
    CHECK(app.groups == direct.groups);
    CHECK(app.platform_id == direct.platform_id);
    entries = (uintptr_t *)(void *)((uint8_t *)&app + offsetof(aar_api, exit));
    for (i = 0; i < zzapp_svc_api_count(); ++i) {
        if (i == api_slot(offsetof(aar_api, audio_stats))) {
            CHECK(entries[i] == 0U);
        } else {
            CHECK(zzapp_svc_is_veneer(entries[i]));
            CHECK(entries[i] == zzapp_svc_api_veneer(i));
        }
    }

    /* A table that ends before the slots a later minor appended: the app
     * gates on `size`, and the slots it must not call still hold veneers. */
    direct.size = (uint32_t)offsetof(aar_api, ext);
    CHECK(zzapp_svc_copy_api(&app, &direct) == AAR_OK);
    CHECK(app.size == offsetof(aar_api, ext));
    for (i = 0; i < zzapp_svc_api_count(); ++i)
        CHECK(zzapp_svc_is_veneer(entries[i]) ||
              entries[i] == 0U);
    CHECK(zzapp_svc_copy_api(&app, NULL) == AAR_E_INVAL);
    direct.size = (uint32_t)(sizeof(direct) + 8U);
    CHECK(zzapp_svc_copy_api(&app, &direct) == AAR_E_INVAL);
}

static void test_gl_table_copy_preserves_missing_calls(void)
{
    aar_gl direct;
    aar_gl app;
    uintptr_t *entries;
    uint32_t i;

    memset(&direct, 0, sizeof(direct));
    direct.size = sizeof(direct);
    direct.version = AAR_GL_VERSION_11;
    direct.Begin = (void *)(uintptr_t)0x12345678U;
    direct.ClearDepth = (void *)(uintptr_t)0x12345678U;
    CHECK(zzapp_svc_copy_gl(&app, &direct) == AAR_OK);
    CHECK(app.size == direct.size);
    CHECK(app.version == AAR_GL_VERSION_11);
    entries = (uintptr_t *)(void *)((uint8_t *)&app + offsetof(aar_gl, Begin));
    for (i = 0; i < zzapp_svc_gl_count(); ++i) {
        if (i == gl_slot(offsetof(aar_gl, Begin)) ||
            i == gl_slot(offsetof(aar_gl, ClearDepth)))
            CHECK(entries[i] == zzapp_svc_gl_veneer(i));
        else
            CHECK(entries[i] == 0U);
    }
}

static void test_descriptors_cover_callable_slots(void)
{
    uint32_t i;

    CHECK(zzapp_svc_api_count() == AAR_API_SLOT_COUNT);
    CHECK(api_slot(offsetof(aar_api, requester)) == 83U);
    for (i = 0U; i < zzapp_svc_api_count(); ++i)
        CHECK(zzapp_svc_api_descriptor(i) != NULL);
    for (i = 0U; i < zzapp_svc_gl_count(); ++i)
        CHECK(zzapp_svc_gl_descriptor(i) != NULL);
    /* One veneer past the table: aar_ext_zz9000.gl_bind_flags. */
    CHECK(ZZAPP_SVC_EXT_GL_BIND_FLAGS == zzapp_svc_api_count());
    CHECK(zzapp_svc_api_descriptor(ZZAPP_SVC_EXT_GL_BIND_FLAGS) != NULL);
    CHECK(zzapp_svc_is_veneer(zzapp_svc_api_veneer(ZZAPP_SVC_EXT_GL_BIND_FLAGS)));
    CHECK(zzapp_svc_api_descriptor(ZZAPP_SVC_EXT_GL_BIND_FLAGS + 1U) == NULL);
    CHECK(zzapp_svc_api_veneer(ZZAPP_SVC_EXT_GL_BIND_FLAGS + 1U) == 0U);
}


static void test_descriptor_shapes(void)
{
    const zzapp_svc_descriptor_t *api;
    const zzapp_svc_descriptor_t *gl;

    api = zzapp_svc_api_descriptor((uint32_t)((offsetof(aar_api, file_open) -
                                               offsetof(aar_api, exit)) /
                                              sizeof(void *)));
    CHECK(api->arguments == 2U);
    CHECK(api->pointers[0].direction == ZZAPP_SVC_POINTER_IN);
    CHECK(api->pointers[0].length == ZZAPP_SVC_LENGTH_STRING);
    api = zzapp_svc_api_descriptor((uint32_t)((offsetof(aar_api, display_open) -
                                               offsetof(aar_api, exit)) /
                                              sizeof(void *)));
    CHECK(api->arguments == 5U);
    CHECK(api->pointers[0].direction == ZZAPP_SVC_POINTER_OUT);
    CHECK(api->pointers[0].element_bytes == sizeof(aar_surface));
    api = zzapp_svc_api_descriptor((uint32_t)((offsetof(aar_api, event_poll) -
                                               offsetof(aar_api, exit)) /
                                              sizeof(void *)));
    CHECK(api->pointers[0].length == ZZAPP_SVC_LENGTH_COUNT);
    CHECK(api->pointers[0].element_bytes == sizeof(aar_event));
    api = zzapp_svc_api_descriptor((uint32_t)((offsetof(aar_api, time_us) -
                                               offsetof(aar_api, exit)) /
                                              sizeof(void *)));
    CHECK(api->result == ZZAPP_SVC_RETURN_U64);
    api = zzapp_svc_api_descriptor((uint32_t)((offsetof(aar_api, yield) -
                                               offsetof(aar_api, exit)) /
                                              sizeof(void *)));
    CHECK(api->result == ZZAPP_SVC_RETURN_VOID);
    gl = zzapp_svc_gl_descriptor((uint32_t)((offsetof(aar_gl, Vertex2f) -
                                             offsetof(aar_gl, Begin)) /
                                            sizeof(void *)));
    CHECK(gl->arguments == 0U);
    CHECK(gl->vfp_words == 2U);
    api = zzapp_svc_api_descriptor((uint32_t)((offsetof(aar_api, should_stop) -
                                               offsetof(aar_api, exit)) /
                                              sizeof(void *)));
    CHECK(api->flags == ZZAPP_SVC_FAST_NO_LOCK);
    api = zzapp_svc_api_descriptor((uint32_t)((offsetof(aar_api, time_us) -
                                               offsetof(aar_api, exit)) /
                                              sizeof(void *)));
    CHECK(api->flags == ZZAPP_SVC_FAST_NO_LOCK);
    api = zzapp_svc_api_descriptor((uint32_t)((offsetof(aar_api, heap_info) -
                                               offsetof(aar_api, exit)) /
                                              sizeof(void *)));
    CHECK(api->flags == 0U);
    /* A uint64_t takes an even register pair or an 8-aligned stack pair, so
     * a call with one after an odd number of words carries a padding word. */
    api = api_descriptor(offsetof(aar_api, heap_grow));
    CHECK(api->arguments == 3U);
    CHECK(api->pointers[0].argument == 2U);
    api = api_descriptor(offsetof(aar_api, shared_alloc));
    CHECK(api->arguments == 5U);
    CHECK(api->pointers[0].argument == 4U);
    CHECK(api->pointers[0].direction == ZZAPP_SVC_POINTER_OUT);
    CHECK(api->pointers[0].element_bytes == sizeof(aar_shared));
    api = api_descriptor(offsetof(aar_api, publish));
    CHECK(api->arguments == 4U);
    api = api_descriptor(offsetof(aar_api, file_seek));
    CHECK(api->arguments == 6U);
    CHECK(api->pointers[0].argument == 5U);
    CHECK(api->pointers[0].element_bytes == sizeof(int64_t));
    api = api_descriptor(offsetof(aar_api, sha256));
    CHECK(api->arguments == 5U);
    CHECK(api->pointers[0].argument == 0U);
    CHECK(api->pointers[0].length == ZZAPP_SVC_LENGTH_ARGUMENT64);
    CHECK(api->pointers[0].length_argument == 2U);
    CHECK(api->pointers[1].argument == 4U);
    CHECK(api->pointers[1].element_bytes == AAR_SHA256_BYTES);
    api = api_descriptor(offsetof(aar_api, decompress));
    CHECK(api->arguments == 11U);
    CHECK(api->pointers[0].argument == 2U);
    CHECK(api->pointers[0].length == ZZAPP_SVC_LENGTH_ARGUMENT64);
    CHECK(api->pointers[0].length_argument == 4U);
    CHECK(api->pointers[1].argument == 6U);
    CHECK(api->pointers[1].direction == ZZAPP_SVC_POINTER_OUT);
    CHECK(api->pointers[1].length == ZZAPP_SVC_LENGTH_ARGUMENT64);
    CHECK(api->pointers[1].length_argument == 8U);
    CHECK(api->pointers[2].argument == 10U);
    CHECK(api->pointers[2].element_bytes == sizeof(aar_service_result));
    api = api_descriptor(offsetof(aar_api, info));
    CHECK(api->pointers[0].length == ZZAPP_SVC_LENGTH_SIZE_FIELD);
    CHECK(api->pointers[0].element_bytes == sizeof(aar_info));
    api = api_descriptor(offsetof(aar_api, audio_stats));
    CHECK(api->pointers[0].length == ZZAPP_SVC_LENGTH_SIZE_FIELD);
    CHECK(api->pointers[0].element_bytes == sizeof(aar_audio_stats));
    api = api_descriptor(offsetof(aar_api, ext));
    CHECK(api->arguments == 2U);
    CHECK(api->result == ZZAPP_SVC_RETURN_POINTER);
    api = zzapp_svc_api_descriptor(ZZAPP_SVC_EXT_GL_BIND_FLAGS);
    CHECK(api->arguments == 2U);
    CHECK(api->result == ZZAPP_SVC_RETURN_POINTER);
}

static void test_pointer_checks(void)
{
    zzapp_as_t as;
    const uint32_t *l2;

    setup(&as);
    l2 = zzapp_as_l2_for(&as, WINDOW);
    CHECK(l2 != NULL);
    CHECK((l2[0] & (3U << 4)) == (3U << 4));
    CHECK((l2[1] & (3U << 4)) == (2U << 4));
    CHECK(zzapp_svc_check_user(&as, WINDOW, 4096U, 1) == AAR_OK);
    CHECK(zzapp_svc_check_user(&as, WINDOW + 4095U, 2U, 1) == AAR_E_FAULT);
    CHECK(zzapp_svc_check_user(&as, WINDOW + 0x1000U, 4U, 1) == AAR_E_FAULT);
    CHECK(zzapp_svc_check_user(&as, WINDOW + 0x1000U, 4U, 0) == AAR_OK);
    CHECK(zzapp_svc_check_user(&as, user_va, 4U, 1) == AAR_OK);
    CHECK(zzapp_svc_check_user(&as, 0x02000000U, 4U, 1) == AAR_E_FAULT);
    CHECK(zzapp_svc_check_user(&as, 0xfffffff0U, 32U, 0) == AAR_E_FAULT);
    zzapp_as_destroy(&as);
}

static void test_large_and_section_pointer_checks(void)
{
    zzapp_as_t as;
    const uint32_t frame = 0x01200000U;
    const uint32_t *l2;

    setup(&as);
    CHECK(zzapp_as_map_image(&as, WINDOW + 0x00010000U, 0x01100000U,
                             0x10000U, ZZAPP_AS_IMAGE_RX) == 0);
    l2 = zzapp_as_l2_for(&as, WINDOW + 0x00010000U);
    CHECK(l2 != NULL);
    CHECK((l2[0x10U] & 3U) == 1U);
    CHECK(zzapp_as_map_frames(&as, WINDOW + 0x00100000U, &frame, 1U) == 0);
    CHECK(zzapp_svc_check_user(&as, WINDOW + 0x00012345U, 4U, 0) == AAR_OK);
    CHECK(zzapp_svc_check_user(&as, WINDOW + 0x00012345U, 4U, 1) == AAR_E_FAULT);
    CHECK(zzapp_svc_check_user(&as, WINDOW + 0x00101234U, 4U, 1) == AAR_OK);
    CHECK(zzapp_svc_check_user(&as, WINDOW + 0x00011ff0U, 32U, 0) == AAR_OK);
    CHECK(zzapp_svc_check_user(&as, WINDOW + 0x00011ff0U, 32U, 1) == AAR_E_FAULT);
    CHECK(zzapp_svc_check_user(&as, WINDOW + 0x00080000U, 4U, 0) == AAR_E_FAULT);
    zzapp_as_destroy(&as);
}

static const char *app_gl_string(uint32_t alias, uint32_t name)
{
    zzapp_svc_frame_t frame;
    uint32_t offset;

    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, GetString)),
             &frame, (uint32_t[9]){ name });
    if (!frame.r[0])
        return NULL;
    offset = frame.r[0] - alias;
    CHECK(offset < ZZAPP_REC_GL_PAGE_OFFSET);
    if (offset >= ZZAPP_REC_GL_PAGE_OFFSET)
        return NULL;
    return (const char *)zzapp_svc_gl_for_slot(0U) + offset;
}

static void test_gl_strings_survive_other_queries(void)
{
    const uint32_t alias = 0xd8001000U;
    zzapp_svc_frame_t frame;
    const char *vendor, *renderer, *version, *extensions;

    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    gl.GetString = mock_get_string;
    mock_renderer = "renderer A";
    CHECK(zzapp_svc_set_table_aliases(0U, alias - 4096U, alias) == AAR_OK);
    dispatch(api_slot(offsetof(aar_api, gl_bind)), &frame, (uint32_t[9]){ 1U });
    CHECK(frame.r[0] == alias);

    vendor = app_gl_string(alias, 0x1f00U);
    renderer = app_gl_string(alias, 0x1f01U);
    version = app_gl_string(alias, 0x1f02U);
    extensions = app_gl_string(alias, 0x1f03U);
    CHECK(vendor && strcmp(vendor, "test vendor") == 0);
    CHECK(renderer && strcmp(renderer, "renderer A") == 0);
    CHECK(version && strcmp(version, "1.1") == 0);
    CHECK(extensions && *extensions == '\0');
    CHECK(app_gl_string(alias, 0x1f04U) == NULL);

    dispatch(api_slot(offsetof(aar_api, gl_unbind)), &frame, (uint32_t[9]){ 0U });
    mock_renderer = "renderer B";
    dispatch(api_slot(offsetof(aar_api, gl_bind)), &frame, (uint32_t[9]){ 1U });
    CHECK(frame.r[0] == alias);
    renderer = app_gl_string(alias, 0x1f01U);
    CHECK(renderer && strcmp(renderer, "renderer B") == 0);
    teardown_dispatch();
}

static void test_gl_deferred_pointer_checks(void)
{
    zzapp_svc_frame_t frame;
#if ZZAPP_PERF_COUNTERS
    zzapp_gl_telemetry_snapshot_t snapshot;
#endif

    uint32_t args[9] = { 0U };
    uint32_t extra_va;

    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    dispatch(api_slot(offsetof(aar_api, gl_bind)), &frame, (uint32_t[9]){ 1U });
    CHECK(frame.r[0] != 0U);
#if ZZAPP_PERF_COUNTERS
    CHECK(zzapp_svc_gl_telemetry_swap(0U, 0U, NULL) == 0);
#endif

    CHECK(zzapp_as_alias_map(&proc.as, 0x02001000U, 4096U, 1,
                             &extra_va) == AAR_OK);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, VertexPointer)),
             &frame, (uint32_t[9]){ 3U, 0x1406U, 0U, user_va + 0x100U });
    CHECK(vertex_pointer_calls == 1U);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, EnableClientState)),
             &frame, (uint32_t[9]){ 0x8074U });
    CHECK(zzapp_as_alias_unmap(&proc.as, extra_va, 4096U) == AAR_OK);
    args[0] = 4U;
    args[1] = 0U;
    args[2] = 1024U;
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, DrawArrays)),
             &frame, args);
    CHECK(draw_arrays_calls == 0U);
    CHECK(frame.r[0] == (uint32_t)AAR_E_FAULT);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, GetError)),
             &frame, (uint32_t[9]){ 0U });
    CHECK(frame.r[0] == 0x0501U);
    CHECK(zzapp_as_alias_map(&proc.as, 0x02001000U, 12288U, 1,
                             &extra_va) == AAR_OK);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, DrawArrays)),
             &frame, args);
    CHECK(draw_arrays_calls == 1U);
    CHECK(zzapp_as_alias_unmap(&proc.as, extra_va, 12288U) == AAR_OK);
    *(uint16_t *)(void *)(user_page + 0x700U) = 1024U;
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, DrawElements)),
             &frame, (uint32_t[9]){ 4U, 1U, 0x1403U, user_va + 0x700U });
    CHECK(draw_elements_calls == 0U);
    CHECK(frame.r[0] == (uint32_t)AAR_E_FAULT);

    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, TexImage2D)),
             &frame, (uint32_t[9]){ 0x0de1U, 0U, 4U, 32U, 32U, 0U,
                                    0x1908U, 0x1401U, extra_va });
    CHECK(texture_upload_calls == 0U);
    CHECK(frame.r[0] == (uint32_t)AAR_E_FAULT);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, ReadPixels)),
             &frame, (uint32_t[9]){ 0U, 0U, 1U, 1U, 0x1908U, 0x1401U,
                                    WINDOW + 0x1000U });
    CHECK(read_pixels_calls == 0U);
    CHECK(frame.r[0] == (uint32_t)AAR_E_FAULT);
    dispatch(ZZAPP_SVC_GL_FIRST + zzapp_svc_gl_count(), &frame,
             (uint32_t[9]){ 0U });
    CHECK(frame.r[0] == (uint32_t)AAR_E_NOSYS);
#if ZZAPP_PERF_COUNTERS
    CHECK(zzapp_svc_gl_telemetry_swap(0U, 999999U, &snapshot) == 0);
    CHECK(snapshot.descriptor_failures == 1U);
    CHECK(snapshot.pointer_failures >= 4U);
    CHECK(snapshot.array_span_checks >= 2U);
#endif
    teardown_dispatch();
}
static void test_gl_span_validation(void)
{
    zzapp_svc_frame_t frame;
#if ZZAPP_PERF_COUNTERS
    zzapp_gl_telemetry_snapshot_t snapshot;
#endif
    uint32_t extra_va;
    void *index_pages;
    uint32_t i;

    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    dispatch(api_slot(offsetof(aar_api, gl_bind)), &frame, (uint32_t[9]){ 1U });
    CHECK(zzapp_as_alias_map(&proc.as, 0x02001000U, 0x20000U, 1,
                             &extra_va) == AAR_OK);
    index_pages = mmap((void *)(uintptr_t)(user_va + 0x2000U), 0x3000U,
                       PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS |
                       MAP_FIXED, -1, 0);
    CHECK(index_pages != MAP_FAILED);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, VertexPointer)),
             &frame, (uint32_t[9]){ 3U, 0x1406U, 12U, extra_va });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, ColorPointer)),
             &frame, (uint32_t[9]){ 4U, 0x1406U, 16U, extra_va + 0x8000U });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, TexCoordPointer)),
             &frame, (uint32_t[9]){ 2U, 0x1406U, 8U, extra_va + 0x10000U });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, EnableClientState)),
             &frame, (uint32_t[9]){ 0x8074U });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, EnableClientState)),
             &frame, (uint32_t[9]){ 0x8076U });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, EnableClientState)),
             &frame, (uint32_t[9]){ 0x8078U });
    for (i = 0U; i < 6144U; ++i)
        ((uint16_t *)(void *)(user_page + 0x700U))[i] = (uint16_t)(i & 1023U);
#if ZZAPP_PERF_COUNTERS
    zzapp_svc_gl_telemetry_reset(0U);
    CHECK(zzapp_svc_gl_telemetry_swap(0U, 0U, NULL) == 0);
#endif
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, DrawElements)),
             &frame, (uint32_t[9]){ 4U, 6144U, 0x1403U, user_va + 0x700U });
    CHECK(draw_elements_calls == 1U);
#if ZZAPP_PERF_COUNTERS
    CHECK(zzapp_svc_gl_telemetry_swap(0U, 1U, &snapshot) == 0);
    CHECK(snapshot.array_span_checks == 3U);
    CHECK(snapshot.array_span_fallbacks == 0U);
#endif
    if (index_pages != MAP_FAILED)
        munmap(index_pages, 0x3000U);
    CHECK(zzapp_as_alias_unmap(&proc.as, extra_va, 0x20000U) == AAR_OK);
    teardown_dispatch();
}
static void test_gl_span_fallback_reference(void)
{
    zzapp_svc_frame_t frame;
    reference_array_t arrays[3];
    uint32_t gap_va;
    uint32_t target_va;
    uint32_t values[4] = { 0U, 1U, 2U, 3U };
    uint32_t seed = 0x4d595df4U;
    uint32_t iteration;
    unsigned before;

    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    dispatch(api_slot(offsetof(aar_api, gl_bind)), &frame, (uint32_t[9]){ 1U });
    CHECK(zzapp_as_alias_map(&proc.as, 0x02001000U, 4096U, 1, &gap_va) == AAR_OK);
    CHECK(zzapp_as_alias_map(&proc.as, 0x02002000U, 4096U, 1, &target_va) == AAR_OK);
    CHECK(zzapp_as_alias_unmap(&proc.as, gap_va, 4096U) == AAR_OK);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, VertexPointer)),
             &frame, (uint32_t[9]){ 3U, 0x1406U, 8192U, user_va });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, EnableClientState)),
             &frame, (uint32_t[9]){ 0x8074U });
    ((uint16_t *)(void *)(user_page + 0x700U))[0] = 0U;
    ((uint16_t *)(void *)(user_page + 0x700U))[1] = 1U;
    before = draw_elements_calls;
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, DrawElements)),
             &frame, (uint32_t[9]){ 4U, 2U, 0x1403U, user_va + 0x700U });
    CHECK(draw_elements_calls == before + 1U);
    ((uint8_t *)(void *)(user_page + 0x700U))[0] = 0U;
    ((uint8_t *)(void *)(user_page + 0x700U))[1] = 1U;
    before = draw_elements_calls;
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, DrawElements)),
             &frame, (uint32_t[9]){ 4U, 2U, 0x1401U, user_va + 0x700U });
    CHECK(draw_elements_calls == before + 1U);
    for (iteration = 0U; iteration < 32U; ++iteration) {
        uint32_t array;
        int expected;

        seed = seed * 1664525U + 1013904223U;
        for (array = 0U; array < 3U; ++array) {
            static const uint32_t types[] = { 0x1401U, 0x1403U, 0x1406U };
            static const uint32_t strides[] = { 0U, 4U, 16U, 8192U };

            seed = seed * 1664525U + 1013904223U;
            arrays[array].pointer = (seed & 1U) ? user_va + (seed & 63U) :
                                                   target_va + (seed & 63U);
            arrays[array].size = (int32_t)(1U + ((seed >> 1) & 3U));
            arrays[array].type = types[(seed >> 3) % 3U];
            arrays[array].stride = (int32_t)strides[(seed >> 5) % 4U];
            arrays[array].enabled = (uint8_t)((seed >> 7) & 1U);
        }
        dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, VertexPointer)),
                 &frame, (uint32_t[9]){ (uint32_t)arrays[0].size, arrays[0].type,
                                         (uint32_t)arrays[0].stride, arrays[0].pointer });
        dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, ColorPointer)),
                 &frame, (uint32_t[9]){ (uint32_t)arrays[1].size, arrays[1].type,
                                         (uint32_t)arrays[1].stride, arrays[1].pointer });
        dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, TexCoordPointer)),
                 &frame, (uint32_t[9]){ (uint32_t)arrays[2].size, arrays[2].type,
                                         (uint32_t)arrays[2].stride, arrays[2].pointer });
        CHECK(frame.r[0] != (uint32_t)AAR_E_NOSYS);

        for (array = 0U; array < 3U; ++array) {
            uint32_t state = array == 0U ? 0x8074U :
                             array == 1U ? 0x8076U : 0x8078U;
            uint32_t number = arrays[array].enabled ?
                gl_slot(offsetof(aar_gl, EnableClientState)) :
                gl_slot(offsetof(aar_gl, DisableClientState));

            dispatch(ZZAPP_SVC_GL_FIRST + number, &frame, (uint32_t[9]){ state });
            CHECK(frame.r[0] != (uint32_t)AAR_E_NOSYS);
        }
        if (iteration == 0U)
            values[0] = UINT32_MAX;
        else
            values[0] = iteration & 3U;
        ((uint32_t *)(void *)(user_page + 0x600U))[0] = values[0];
        ((uint32_t *)(void *)(user_page + 0x600U))[1] = values[1];
        ((uint32_t *)(void *)(user_page + 0x600U))[2] = values[2];
        ((uint32_t *)(void *)(user_page + 0x600U))[3] = values[3];
        expected = reference_draw_accepts(&proc.as, arrays, values, 4U);
        before = draw_elements_calls;
        dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, DrawElements)),
                 &frame, (uint32_t[9]){ 4U, 4U, 0x1405U, user_va + 0x600U });
        CHECK((draw_elements_calls != before) == expected);
    }
    CHECK(zzapp_as_alias_unmap(&proc.as, target_va, 4096U) == AAR_OK);
    teardown_dispatch();
}

/* A client-array pointer is validated when the array is drawn, not when it is
 * set: a bad pointer reaches the module, the draw is refused with
 * AAR_E_FAULT and glGetError says GL_INVALID_VALUE once. */
static void test_gl_bad_array_pointer_fails_at_draw(void)
{
    zzapp_svc_frame_t frame;
    uint32_t extra_va;
    unsigned bad = 0U;

#define ARRAY_CHECK(expr) do { checks++; if (!(expr)) { failures++; bad++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); } } while (0)
    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    dispatch(api_slot(offsetof(aar_api, gl_bind)), &frame, (uint32_t[9]){ 1U });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, VertexPointer)),
             &frame, (uint32_t[9]){ 3U, 0x1406U, 0U, 0x10U });
    ARRAY_CHECK(vertex_pointer_calls == 1U);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, EnableClientState)),
             &frame, (uint32_t[9]){ 0x8074U });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, GetError)),
             &frame, (uint32_t[9]){ 0U });
    ARRAY_CHECK(frame.r[0] == 0U);             /* setting it is no error */
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, DrawArrays)),
             &frame, (uint32_t[9]){ 4U, 0U, 3U });
    ARRAY_CHECK(draw_arrays_calls == 0U);
    ARRAY_CHECK(frame.r[0] == (uint32_t)AAR_E_FAULT);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, GetError)),
             &frame, (uint32_t[9]){ 0U });
    ARRAY_CHECK(frame.r[0] == 0x0501U);        /* GL_INVALID_VALUE */
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, GetError)),
             &frame, (uint32_t[9]){ 0U });
    ARRAY_CHECK(frame.r[0] == 0U);             /* read once */

    /* A range that starts in mapped memory and runs past its end: the first
     * vertex is drawn, the second is not. */
    ARRAY_CHECK(zzapp_as_alias_map(&proc.as, 0x02001000U, 4096U, 1,
                                   &extra_va) == AAR_OK);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, VertexPointer)),
             &frame, (uint32_t[9]){ 3U, 0x1406U, 0U, extra_va + 4096U - 12U });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, DrawArrays)),
             &frame, (uint32_t[9]){ 4U, 0U, 1U });
    ARRAY_CHECK(draw_arrays_calls == 1U);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, DrawArrays)),
             &frame, (uint32_t[9]){ 4U, 0U, 2U });
    ARRAY_CHECK(draw_arrays_calls == 1U);
    ARRAY_CHECK(frame.r[0] == (uint32_t)AAR_E_FAULT);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, GetError)),
             &frame, (uint32_t[9]){ 0U });
    ARRAY_CHECK(frame.r[0] == 0x0501U);
    ARRAY_CHECK(zzapp_as_alias_unmap(&proc.as, extra_va, 4096U) == AAR_OK);
    printf("svc_test: bad array pointer: %u failures\n", bad);
#undef ARRAY_CHECK
    teardown_dispatch();
}

static void test_gl_lock_calls_stay_kernel_side(void)
{
    zzapp_svc_frame_t frame;
    const aar_gl *table;
    unsigned before;

    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    dispatch(api_slot(offsetof(aar_api, gl_bind)), &frame, (uint32_t[9]){ 1U });
    table = zzapp_svc_gl_for_slot(0U);
    CHECK(table != NULL);
    CHECK(zzapp_svc_is_veneer((uintptr_t)table->LockArrays));
    CHECK(zzapp_svc_is_veneer((uintptr_t)table->UnlockArrays));

    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, UnlockArrays)),
             &frame, (uint32_t[9]){ 0U });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, GetError)),
             &frame, (uint32_t[9]){ 0U });
    CHECK(frame.r[0] == 0x0502U);

    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, VertexPointer)),
             &frame, (uint32_t[9]){ 3U, 0x1406U, 12U, user_va + 0x100U });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, EnableClientState)),
             &frame, (uint32_t[9]){ 0x8074U });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, LockArrays)),
             &frame, (uint32_t[9]){ 0U, 1U });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, GetError)),
             &frame, (uint32_t[9]){ 0U });
    CHECK(frame.r[0] == 0x0502U);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, UnlockArrays)),
             &frame, (uint32_t[9]){ 0U });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, GetError)),
             &frame, (uint32_t[9]){ 0U });
    CHECK(frame.r[0] == 0x0502U);

    mock_vertex_array_enabled = 1U;
    mock_array_transformations_enabled = 0U;
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, LockArrays)),
             &frame, (uint32_t[9]){ 0U, 1U });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, GetError)),
             &frame, (uint32_t[9]){ 0U });
    CHECK(frame.r[0] == 0x0502U);

    mock_array_transformations_enabled = 1U;
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, LockArrays)),
             &frame, (uint32_t[9]){ 0U, 0U });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, GetError)),
             &frame, (uint32_t[9]){ 0U });
    CHECK(frame.r[0] == 0U);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, UnlockArrays)),
             &frame, (uint32_t[9]){ 0U });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, GetError)),
             &frame, (uint32_t[9]){ 0U });
    CHECK(frame.r[0] == 0U);

    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, LockArrays)),
             &frame, (uint32_t[9]){ UINT32_MAX, 1U });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, GetError)),
             &frame, (uint32_t[9]){ 0U });
    CHECK(frame.r[0] == 0x0501U);

    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, LockArrays)),
             &frame, (uint32_t[9]){ 0U, INT32_MAX });
    CHECK(lock_arrays_calls == 0U);
    ((uint16_t *)(void *)(user_page + 0x700U))[0] = 0U;
    before = draw_elements_calls;
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, DrawElements)),
             &frame, (uint32_t[9]){ 4U, 1U, 0x1403U, user_va + 0x700U });
    CHECK(draw_elements_calls == before + 1U);
    ((uint16_t *)(void *)(user_page + 0x700U))[0] = 1024U;
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, DrawElements)),
             &frame, (uint32_t[9]){ 4U, 1U, 0x1403U, user_va + 0x700U });
    CHECK(draw_elements_calls == before + 1U);
    CHECK(frame.r[0] == (uint32_t)AAR_E_FAULT);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, GetError)),
             &frame, (uint32_t[9]){ 0U });
    CHECK(frame.r[0] == 0x0501U);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, UnlockArrays)),
             &frame, (uint32_t[9]){ 0U });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, GetError)),
             &frame, (uint32_t[9]){ 0U });
    CHECK(frame.r[0] == 0U);

    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, Begin)),
             &frame, (uint32_t[9]){ UINT32_MAX });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, GetError)),
             &frame, (uint32_t[9]){ 0U });
    CHECK(frame.r[0] == 0x0500U);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, LockArrays)),
             &frame, (uint32_t[9]){ 0U, 1U });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, End)),
             &frame, (uint32_t[9]){ 0U });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, GetError)),
             &frame, (uint32_t[9]){ 0U });
    CHECK(frame.r[0] == 0x0502U);

    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, Begin)),
             &frame, (uint32_t[9]){ UINT32_MAX });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, GetError)),
             &frame, (uint32_t[9]){ 0U });
    CHECK(frame.r[0] == 0x0500U);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, UnlockArrays)),
             &frame, (uint32_t[9]){ 0U });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, End)),
             &frame, (uint32_t[9]){ 0U });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, GetError)),
             &frame, (uint32_t[9]){ 0U });
    CHECK(frame.r[0] == 0x0502U);

    CHECK(lock_arrays_calls == 0U);
    CHECK(unlock_arrays_calls == 0U);
    teardown_dispatch();
}




static void test_copy_out_and_pixel_argument_positions(void)
{
    zzapp_svc_frame_t frame;

    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    dispatch(api_slot(offsetof(aar_api, gl_bind)), &frame, (uint32_t[9]){ 1U });
    CHECK(frame.r[0] != 0U);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, TexSubImage2D)),
             &frame, (uint32_t[9]){ 0x0de1U, 0U, 0U, 0U, 1U, 1U,
                                    0x1908U, 0x1401U, user_va + 0x100U });
    CHECK(texture_sub_upload_calls == 1U);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, ReadPixels)),
             &frame, (uint32_t[9]){ 0U, 0U, 1U, 1U, 0x1908U, 0x1401U,
                                    user_va + 0x200U });
    CHECK(read_pixels_calls == 1U);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, Build2DMipmaps)),
             &frame, (uint32_t[9]){ 0x0de1U, 4U, 1U, 1U, 0x1908U, 0x1401U,
                                    user_va + 0x240U });
    CHECK(build_mipmaps_calls == 1U);


    proc.heap_base = 0U;
    proc.heap_break = 1U << 20;
    proc.heap_frame_count = 1U;
    dispatch(api_slot(offsetof(aar_api, heap_shrink)), &frame,
             (uint32_t[9]){ 1U << 20, 0U, WINDOW + 0x100U });
    CHECK(frame.r[0] == (uint32_t)AAR_E_FAULT);
    CHECK(heap_shrink_calls == 0U);
    teardown_dispatch();
}
static void test_extended_pointer_checks(void)
{
    zzapp_svc_frame_t frame;
    uint8_t *message;

    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    dispatch(api_slot(offsetof(aar_api, gl_bind)), &frame, (uint32_t[9]){ 1U });
    CHECK(frame.r[0] != 0U);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, GetIntegerv)),
             &frame, (uint32_t[9]){ 0x0ba2U, user_va + 0xffcU });
    CHECK(getter_calls == 0U);
    CHECK(frame.r[0] == (uint32_t)AAR_E_FAULT);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, PixelStorei)),
             &frame, (uint32_t[9]){ 0x0d04U, 1024U });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, ReadPixels)),
             &frame, (uint32_t[9]){ 0U, 0U, 1U, 1U, 0x1908U, 0x1401U,
                                    user_va + 0xffcU });
    CHECK(read_pixels_calls == 0U);
    CHECK(frame.r[0] == (uint32_t)AAR_E_FAULT);
    message = user_page + 0x600U;
    memset(message, 0xa5, 32U);
    dispatch(api_slot(offsetof(aar_api, msg_poll)), &frame,
             (uint32_t[9]){ user_va + 0x600U });
    CHECK(frame.r[0] == 1U);
    CHECK(msg_poll_calls == 1U);
    CHECK(message[0] == 0U);
    dispatch(api_slot(offsetof(aar_api, heap_range)), &frame,
             (uint32_t[9]){ 0U, 0U });
    CHECK(heap_range_calls == 1U);
    dispatch(api_slot(offsetof(aar_api, file_seek)), &frame,
             (uint32_t[9]){ 1U, 0U, 0U, 0U, 0U, 0U });
    CHECK(file_seek_calls == 1U);
    proc.generation = 1U;
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, Begin)),
             &frame, (uint32_t[9]){ 4U });
    CHECK(gl_begin_calls == 0U);
    proc.generation = 0U;
    zzapp_svc_gl_release(0U, 0U);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, Begin)),
             &frame, (uint32_t[9]){ 4U });
    CHECK(gl_begin_calls == 0U);
    CHECK(frame.r[0] == (uint32_t)AAR_E_NOSYS);
    CHECK(frame.r[0] == (uint32_t)AAR_E_NOSYS);
    teardown_dispatch();
}
static void test_svc_uses_core_aware_slot_resolver(void)
{
    zzapp_svc_frame_t frame;

    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    zzapp_kernel_set_current_thread(1);
    dispatch(api_slot(offsetof(aar_api, gl_bind)), &frame,
             (uint32_t[9]){ 1U });
    CHECK(frame.r[0] != (uint32_t)AAR_E_FAULT);
    zzapp_kernel_set_current_thread(0);
    teardown_dispatch();
}
static void test_gl_getters_copy_full_outputs(void)
{
    zzapp_svc_frame_t frame;
    static const uint32_t vector_pnames[] = {
        0x0b66U, 0x0c23U, 0x0b53U
    };
    uint32_t i;

    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    dispatch(api_slot(offsetof(aar_api, gl_bind)), &frame, (uint32_t[9]){ 1U });
    for (i = 0U; i < sizeof(vector_pnames) / sizeof(vector_pnames[0]); ++i) {
        dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, GetIntegerv)),
                 &frame, (uint32_t[9]){ vector_pnames[i], user_va + 0xffcU });
        CHECK(getter_calls == 0U);
        CHECK(frame.r[0] == (uint32_t)AAR_E_FAULT);
    }
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, GetIntegerv)),
             &frame, (uint32_t[9]){ vector_pnames[0], user_va + 0x500U });
    CHECK(getter_calls == 1U);
    CHECK(getter_pointer != (uintptr_t)(user_va + 0x500U));
    teardown_dispatch();
}

static void test_minor6_gl_pointer_extents(void)
{
    zzapp_svc_frame_t frame;
    uint32_t getd = ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, GetDoublev));
    uint32_t getb = ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, GetBooleanv));
    uint32_t light = ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, Lightfv));

    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    gl.GetDoublev = (void (*)(uint32_t, double *))mock_get_integerv;
    gl.GetBooleanv = (void (*)(uint32_t, uint8_t *))mock_get_integerv;
    dispatch(api_slot(offsetof(aar_api, gl_bind)), &frame, (uint32_t[9]){ 1U });

    /* Sixteen doubles do not fit in the last 64 bytes of the page.
     * Four GLbooleans do fit in its last four bytes. */
    dispatch(getd, &frame, (uint32_t[9]){ 0x0ba6U, user_va + 0xfc0U });
    CHECK(frame.r[0] == (uint32_t)AAR_E_FAULT && getter_calls == 0U);
    dispatch(getb, &frame, (uint32_t[9]){ 0x0c23U, user_va + 0xffcU });
    CHECK(getter_calls == 1U);
    /* GL_POSITION is four floats; a one-float pointer is not enough. */
    dispatch(light, &frame, (uint32_t[9]){ 0x4000U, 0x1203U, user_va + 0xffcU });
    CHECK(frame.r[0] == (uint32_t)AAR_E_FAULT);
    /* A direct SVC for an absent hard-float slot may not call through NULL. */
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, ClearDepth)), &frame,
             (uint32_t[9]){ 0U });
    CHECK(frame.r[0] == (uint32_t)AAR_E_NOSYS);
    teardown_dispatch();
}

static uint32_t clip_plane_name;
static uintptr_t clip_plane_pointer;
#if UINTPTR_MAX == UINT32_MAX
static double clip_plane_equation[4];
#endif

static void mock_clip_plane(uint32_t plane, const double *equation)
{
    clip_plane_name = plane;
    clip_plane_pointer = (uintptr_t)equation;
    /* The host suite otherwise uses A32 words on a 64-bit stack. */
#if UINTPTR_MAX == UINT32_MAX
    if (equation && clip_plane_pointer != user_va + 0xfe0U &&
        clip_plane_pointer != user_va + 0xfe8U)
        memcpy(clip_plane_equation, equation, sizeof(clip_plane_equation));
#endif
}

static void test_clip_plane_copies_four_doubles(void)
{
    const double equation[4] = { -0.25, 1.5, 0.0, -3.0 };
    const uint32_t clip = ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, ClipPlane));
    zzapp_svc_frame_t frame;

    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    gl.ClipPlane = mock_clip_plane;
    clip_plane_name = 0U;
    dispatch(api_slot(offsetof(aar_api, gl_bind)), &frame, (uint32_t[9]){ 1U });

    /* Only three doubles remain mapped here. The callback must not run. */
    dispatch(clip, &frame, (uint32_t[9]){ 0x3000U, user_va + 0xfe8U });
    CHECK(frame.r[0] == (uint32_t)AAR_E_FAULT && clip_plane_name == 0U);
    memcpy(user_page + 0xfe0U, equation, sizeof(equation));
    dispatch(clip, &frame, (uint32_t[9]){ 0x3005U, user_va + 0xfe0U });
    CHECK(clip_plane_name == 0x3005U);
    CHECK(clip_plane_pointer != 0U && clip_plane_pointer != user_va + 0xfe0U);
#if UINTPTR_MAX == UINT32_MAX
    CHECK(memcmp(equation, clip_plane_equation, sizeof(equation)) == 0);
#endif
    teardown_dispatch();
}

static void test_minor6_present_timestamp_uses_both_registers(void)
{
    zzapp_svc_frame_t frame;

    setup_dispatch();
    if (user_page) {
        uint64_t expected = mock_time_us();
        api.display_last_present_us = mock_time_us;
        dispatch(api_slot(offsetof(aar_api, display_last_present_us)), &frame,
                 (uint32_t[9]){ 0U });
        CHECK(frame.r[0] == (uint32_t)expected);
        CHECK(frame.r[1] == (uint32_t)(expected >> 32));
    }
    teardown_dispatch();
}

static unsigned present_calls;
static unsigned present_poll_calls;
static int present_poll_ready;
static const aar_present_result mock_present_result = {
    UINT64_C(0x1122334455667788), UINT64_C(0xaabbccddeeff1122),
    AAR_PRESENT_ACKNOWLEDGED, AAR_PRESENT_REASON_NONE
};

static int mock_present(uint32_t id, uint32_t flags, uint64_t *frame_id)
{
    ++present_calls;
    if (!frame_id)
        return AAR_E_INVAL;
    *frame_id = 0U;
    if (id != 1U || flags != AAR_PRESENT_ASYNC)
        return AAR_E_INVAL;
    *frame_id = mock_present_result.frame_id;
    return AAR_OK;
}

static int mock_present_poll(aar_present_result *out)
{
    ++present_poll_calls;
    if (!out)
        return AAR_E_INVAL;
    if (!present_poll_ready)
        return 0;
    present_poll_ready = 0;
    *out = mock_present_result;
    return 1;
}

static void test_present_validates_full_outputs(void)
{
    const uint32_t submit = api_slot(offsetof(aar_api, display_present));
    const uint32_t poll = api_slot(offsetof(aar_api, display_present_poll));
    zzapp_svc_frame_t frame;
    aar_present_result result;
    uint64_t id;

    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    api.display_present = mock_present;
    api.display_present_poll = mock_present_poll;
    present_calls = present_poll_calls = 0U;
    present_poll_ready = 1;
    memset(user_page + 0x400U, 0xcc, sizeof(result) + 1U);
    dispatch(submit, &frame, (uint32_t[9]){ 1U, AAR_PRESENT_ASYNC, user_va + 0xffcU });
    CHECK(frame.r[0] == (uint32_t)AAR_E_FAULT && present_calls == 0U);
    dispatch(poll, &frame, (uint32_t[9]){ user_va + 0xff0U });
    CHECK(frame.r[0] == (uint32_t)AAR_E_FAULT && present_poll_calls == 0U);
    dispatch(submit, &frame, (uint32_t[9]){ 1U, AAR_PRESENT_ASYNC, user_va + 0x401U });
    CHECK(frame.r[0] == (uint32_t)AAR_E_FAULT && present_calls == 0U);
    dispatch(poll, &frame, (uint32_t[9]){ user_va + 0x401U });
    CHECK(frame.r[0] == (uint32_t)AAR_E_FAULT && present_poll_calls == 0U);
    dispatch(submit, &frame, (uint32_t[9]){ 1U, AAR_PRESENT_ASYNC, 0U });
    CHECK(frame.r[0] == (uint32_t)AAR_E_INVAL);
    dispatch(poll, &frame, (uint32_t[9]){ 0U });
    CHECK(frame.r[0] == (uint32_t)AAR_E_INVAL);
    dispatch(submit, &frame, (uint32_t[9]){ 2U, AAR_PRESENT_ASYNC, user_va + 0x400U });
    CHECK(frame.r[0] == (uint32_t)AAR_E_INVAL);
    memcpy(&id, user_page + 0x400U, sizeof(id));
    CHECK(id == 0U && user_page[0x408U] == 0xccU);
    dispatch(submit, &frame, (uint32_t[9]){ 1U, AAR_PRESENT_ASYNC, user_va + 0x400U });
    CHECK(frame.r[0] == (uint32_t)AAR_OK);
    memcpy(&id, user_page + 0x400U, sizeof(id));
    CHECK(id == mock_present_result.frame_id && user_page[0x408U] == 0xccU);
    dispatch(poll, &frame, (uint32_t[9]){ user_va + 0x400U });
    CHECK(frame.r[0] == 1U);
    memcpy(&result, user_page + 0x400U, sizeof(result));
    CHECK(memcmp(&result, &mock_present_result, sizeof(result)) == 0);
    CHECK(user_page[0x400U + sizeof(result)] == 0xccU);
    memset(user_page + 0x400U, 0xcc, sizeof(result));
    dispatch(poll, &frame, (uint32_t[9]){ user_va + 0x400U });
    CHECK(frame.r[0] == 0U && user_page[0x400U] == 0xccU);
    teardown_dispatch();
}
static void test_rejected_pixel_store_does_not_change_validation(void)
{
    zzapp_svc_frame_t frame;

    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    dispatch(api_slot(offsetof(aar_api, gl_bind)), &frame, (uint32_t[9]){ 1U });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, PixelStorei)),
             &frame, (uint32_t[9]){ 0x0d04U, 1024U });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, Begin)),
             &frame, (uint32_t[9]){ 4U });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, PixelStorei)),
             &frame, (uint32_t[9]){ 0x0d04U, 0U });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, End)),
             &frame, (uint32_t[9]){ 0U });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, ReadPixels)),
             &frame, (uint32_t[9]){ 0U, 0U, 1U, 1U, 0x1908U, 0x1401U,
                                    user_va + 0xffcU });
    CHECK(gl_begin_calls == 1U);
    CHECK(gl_end_calls == 1U);
    CHECK(read_pixels_calls == 0U);
    CHECK(frame.r[0] == (uint32_t)AAR_E_FAULT);
    teardown_dispatch();
}
static void test_svc_number_comes_from_r12(void)
{
    zzapp_svc_frame_t frame;
    uint32_t time = api_slot(offsetof(aar_api, time_us));

    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    *(uint32_t *)(void *)user_page = 0xef000000U | time;
    dispatch(UINT32_MAX, &frame,
             (uint32_t[9]){ 0U });
    CHECK(frame.r[0] == (uint32_t)AAR_E_NOSYS);
    CHECK(time_us_calls == 0U);
    dispatch(time, &frame, (uint32_t[9]){ 0U });
    CHECK(frame.r[0] == UINT32_C(0x55667788));
    CHECK(frame.r[1] == UINT32_C(0x11223344));
    CHECK(time_us_calls == 1U);
    memset(&frame, 0, sizeof(frame));
    frame.r[12] = time;
    frame.user_sp = 0x02000000U;
    zzapp_svc_dispatch(&frame);
    CHECK(frame.r[0] == UINT32_C(0x55667788));
    CHECK(frame.r[1] == UINT32_C(0x11223344));
    CHECK(time_us_calls == 2U);
    teardown_dispatch();
}
static void test_fast_no_lock_calls(void)
{
    zzapp_svc_frame_t frame;

    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    zzapp_kernel_set_current_thread(1);
    dispatch(api_slot(offsetof(aar_api, should_stop)), &frame,
             (uint32_t[9]){ 0U });
    CHECK(frame.r[0] == 1U);
    CHECK(should_stop_calls == 1U);
    CHECK(zzapp_svc_last_number_for(1U) ==
          api_slot(offsetof(aar_api, should_stop)));
    zzapp_kernel_set_current_thread(0);
    teardown_dispatch();
}
#if ZZAPP_SVC_PROBES
#define SVC_PROBE_P2 0x102U
#define SVC_PROBE_P3 0x103U
#define SVC_PROBE_P4 0x104U
static void test_svc_probes_do_not_run_real_handlers(void)
{
    zzapp_svc_frame_t frame;

    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    dispatch(SVC_PROBE_P3, &frame, (uint32_t[9]){ 0U });
    CHECK(frame.r[0] == 0U);
    CHECK(should_stop_calls == 0U);
    dispatch(SVC_PROBE_P4, &frame, (uint32_t[9]){ 0U });
    CHECK(frame.r[0] == 0U);
    CHECK(should_stop_calls == 0U);
    teardown_dispatch();
}
#endif
#if ZZAPP_SVC_PROBES && ZZAPP_PERF_COUNTERS
static void test_svc_probe_shallow_exits_clear_counter_tail(void)
{
    zzapp_svc_frame_t frame;
    uint32_t total_calls = 0U;
    uint32_t samples[4] = { 0U };
    uint32_t averages[4] = { 0U };
    uint32_t call;

    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    zzapp_svc_counters_reset(0U);
    svc_counter_now = 0U;
    for (call = 1U; call <= 64U; ++call) {
        asm_entry_stamp(svc_counter_now);
        dispatch(api_slot(offsetof(aar_api, should_stop)), &frame,
                 (uint32_t[9]){ 0U });
    }
    dispatch(SVC_PROBE_P2, &frame, (uint32_t[9]){ 99U });
    CHECK(frame.r[0] == 0U);
    zzapp_svc_counters_test_eret(0U, svc_counter_now);
    zzapp_svc_counters_test_snapshot(0U, &total_calls, samples, averages);
    CHECK(total_calls == 64U);
    CHECK(samples[3] == 0U);

    zzapp_svc_counters_reset(0U);
    svc_counter_now = 0U;
    for (call = 1U; call <= 64U; ++call) {
        asm_entry_stamp(svc_counter_now);
        dispatch(api_slot(offsetof(aar_api, should_stop)), &frame,
                 (uint32_t[9]){ 0U });
    }
    dispatch(SVC_PROBE_P3, &frame, (uint32_t[9]){ 0U });
    CHECK(frame.r[0] == 0U);
    zzapp_svc_counters_test_eret(0U, svc_counter_now);
    zzapp_svc_counters_test_snapshot(0U, &total_calls, samples, averages);
    CHECK(total_calls == 64U);
    CHECK(samples[3] == 0U);
    teardown_dispatch();
}
#endif
#if ZZAPP_PERF_COUNTERS
static void test_sampled_counter_averages(void)
{
    zzapp_svc_frame_t frame;
    uint32_t total_calls = 0U;
    uint32_t samples[4] = { 0U };
    uint32_t averages[4] = { 0U };
    uint32_t call;

    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    zzapp_svc_counters_reset(0U);
    svc_counter_now = 0U;
    for (call = 1U; call <= 128U; ++call) {
        asm_entry_stamp(svc_counter_now);
        dispatch(api_slot(offsetof(aar_api, should_stop)), &frame,
                 (uint32_t[9]){ 0U });
        if (call == 64U) {
            svc_counter_now += 10U;
            zzapp_svc_counters_test_eret(0U, svc_counter_now);
        }
    }
    zzapp_svc_counters_test_snapshot(0U, &total_calls, samples, averages);
    CHECK(total_calls == 128U);
    CHECK(samples[0] == 2U);
    CHECK(samples[1] == 2U);
    CHECK(samples[2] == 2U);
    CHECK(samples[3] == 1U);
    CHECK(averages[0] == 10U);
    CHECK(averages[1] == 10U);
    CHECK(averages[2] == 10U);
    CHECK(averages[3] == 10U);
    teardown_dispatch();
}
#endif


static void test_svc_float_frame_arguments(void)
{
    zzapp_svc_frame_t frame;
    uint32_t number = ZZAPP_SVC_GL_FIRST +
        gl_slot(offsetof(aar_gl, Vertex2f));

    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    dispatch(api_slot(offsetof(aar_api, gl_bind)), &frame,
             (uint32_t[9]){ 1U });
    memset(&frame, 0, sizeof(frame));
    frame.r[12] = number;
    frame.vfp[0] = UINT32_C(0x3fa00000);
    frame.vfp[1] = UINT32_C(0xc0200000);
    zzapp_svc_dispatch(&frame);
    CHECK(vertex2f_words[0] == UINT32_C(0x3fa00000));
    CHECK(vertex2f_words[1] == UINT32_C(0xc0200000));
    teardown_dispatch();
}

static void test_slot_aliases(void)
{
    const aar_api *table;
    const aar_ext_zz9000 *ext;
    zzapp_svc_frame_t frame;

    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    CHECK(zzapp_svc_set_veneer_alias(0U, 0xd0000000U) == AAR_OK);
    CHECK(zzapp_svc_set_table_aliases(0U, 0xd0001000U, 0xd0002000U) == AAR_OK);
    api.exit = mock_exit;
    table = zzapp_svc_api_for_slot(0U, &api);
    CHECK(table != NULL);
    CHECK((uintptr_t)table->exit == 0xd0000000U);
    ext = zzapp_svc_ext_for_slot(0U);
    CHECK(ext != NULL);
    /* The veneer that follows the last table slot, in the aliased page. */
    CHECK((uintptr_t)ext->gl_bind_flags ==
          0xd0000000U + ZZAPP_SVC_EXT_GL_BIND_FLAGS * 12U);
    CHECK((uintptr_t)ext->build_id == 0xd0001000U +
          offsetof(struct { aar_api table; aar_ext_zz9000 ext;
                            char build_id[128]; }, build_id));
    dispatch(api_slot(offsetof(aar_api, gl_bind)), &frame,
             (uint32_t[9]){ 1U });
    CHECK(frame.r[0] == 0xd0002000U);
    /* aar_ext hands back the aliased ext table. */
    dispatch(api_slot(offsetof(aar_api, ext)), &frame,
             (uint32_t[9]){ AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_MAIN });
    CHECK(frame.r[0] == 0xd0001000U +
          offsetof(struct { aar_api table; aar_ext_zz9000 ext;
                            char build_id[128]; }, ext));
    teardown_dispatch();
}

static void test_ext_table_for_the_app(void)
{
    const aar_ext_zz9000 *ext;
    aar_api bare;

    setup_dispatch();
    CHECK(zzapp_svc_api_for_slot(0U, &api) != NULL);
    ext = zzapp_svc_ext_for_slot(0U);
    CHECK(ext != NULL);
    CHECK(ext->size == sizeof(*ext));
    CHECK(ext->reserved == 0U);
    CHECK(AAR_EXT_ZZ9000_COVERS(ext, gl_build_id));
    CHECK((uintptr_t)ext->gl_bind_flags ==
          zzapp_svc_api_veneer(ZZAPP_SVC_EXT_GL_BIND_FLAGS));
    CHECK(ext->build_id != NULL && strcmp(ext->build_id, "fw-build-0123") == 0);
    /* The app reads its own copy, never firmware memory. */
    CHECK(ext->build_id != runtime_ext.build_id);
    /* The resident firmware does not know the GL module's revision. */
    CHECK(ext->gl_build_id == NULL);

    /* A runtime whose ext table has no GL bind keeps the slot empty. */
    runtime_ext.gl_bind_flags = NULL;
    CHECK(zzapp_svc_api_for_slot(0U, &api) != NULL);
    ext = zzapp_svc_ext_for_slot(0U);
    CHECK(ext != NULL && ext->gl_bind_flags == NULL);

    /* A runtime without ext gives the app no table at all. */
    bare = api;
    bare.ext = NULL;
    CHECK(zzapp_svc_api_for_slot(0U, &bare) != NULL);
    CHECK(zzapp_svc_ext_for_slot(0U) == NULL);
    teardown_dispatch();
}

static void test_ext_call_and_gl_bind_flags(void)
{
    zzapp_svc_frame_t frame;
    const aar_ext_zz9000 *ext;

    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    CHECK(zzapp_svc_api_for_slot(0U, &api) != NULL);
    ext = zzapp_svc_ext_for_slot(0U);
    dispatch(api_slot(offsetof(aar_api, ext)), &frame,
             (uint32_t[9]){ AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_MAIN });
    CHECK(frame.r[0] == (uint32_t)(uintptr_t)ext);
    dispatch(api_slot(offsetof(aar_api, ext)), &frame,
             (uint32_t[9]){ AAR_PLATFORM_NONE, AAR_EXT_ZZ9000_MAIN });
    CHECK(frame.r[0] == 0U);
    dispatch(api_slot(offsetof(aar_api, ext)), &frame,
             (uint32_t[9]){ AAR_PLATFORM_ZZ9000, 2U });
    CHECK(frame.r[0] == 0U);

    /* gl_bind_flags is the veneer after the table slots; it binds like
     * gl_bind and the runtime sees the flags. */
    dispatch(ZZAPP_SVC_EXT_GL_BIND_FLAGS, &frame,
             (uint32_t[9]){ 1U, AAR_ZZ9000_GL_BIND_DIRECT_PRESENT |
                                AAR_ZZ9000_GL_BIND_SYNC_PRESENT });
    CHECK(frame.r[0] == (uint32_t)(uintptr_t)zzapp_svc_gl_for_slot(0U));
    CHECK(ext_bind_flags_seen == 3U);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, Begin)), &frame,
             (uint32_t[9]){ 4U });
    CHECK(gl_begin_calls == 1U);

    /* A surface the runtime refuses leaves nothing bound. */
    dispatch(ZZAPP_SVC_EXT_GL_BIND_FLAGS, &frame, (uint32_t[9]){ 0U, 0U });
    CHECK(frame.r[0] == 0U);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, Begin)), &frame,
             (uint32_t[9]){ 4U });
    CHECK(gl_begin_calls == 1U);
    CHECK(frame.r[0] == (uint32_t)AAR_E_NOSYS);

    /* No ext slot in the runtime's table: the calls are not implemented. */
    api.ext = NULL;
    dispatch(api_slot(offsetof(aar_api, ext)), &frame,
             (uint32_t[9]){ AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_MAIN });
    CHECK(frame.r[0] == (uint32_t)AAR_E_NOSYS);
    dispatch(ZZAPP_SVC_EXT_GL_BIND_FLAGS, &frame, (uint32_t[9]){ 1U, 0U });
    CHECK(frame.r[0] == (uint32_t)AAR_E_NOSYS);
    teardown_dispatch();
}

static void test_service_lengths_are_word_pairs(void)
{
    zzapp_svc_frame_t frame;
    uint32_t words[12];

    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    /* sha256(data, bytes, digest): data in r0, padding in r1, bytes in
     * r2:r3, digest the first stack word. */
    memset(words, 0, sizeof(words));
    words[0] = user_va + 0x100U;
    words[1] = 0xdeadbeefU;
    words[2] = 64U;
    words[4] = user_va + 0x200U;
    memset(user_page + 0x200U, 0xcc, 40U);
    dispatch_words(api_slot(offsetof(aar_api, sha256)), &frame, words, 5U);
    CHECK(frame.r[0] == (uint32_t)AAR_OK);
    CHECK(sha256_calls == 1U && sha256_bytes_seen == 64U);
    /* The digest extent is exactly AAR_SHA256_BYTES: 32 bytes copied back,
     * the rest of the caller's memory untouched. */
    CHECK(user_page[0x200U] == 0U && user_page[0x21fU] == 0U);
    CHECK(user_page[0x220U] == 0xccU);

    /* A length above 32 bits is not forwarded and is not a fault. */
    words[3] = 1U;
    dispatch_words(api_slot(offsetof(aar_api, sha256)), &frame, words, 5U);
    CHECK(frame.r[0] == (uint32_t)AAR_E_INVAL);
    CHECK(sha256_calls == 1U);

    /* A 32-bit length no address space holds is a bad pointer. */
    words[2] = 0xfffff000U;
    words[3] = 0U;
    dispatch_words(api_slot(offsetof(aar_api, sha256)), &frame, words, 5U);
    CHECK(frame.r[0] == (uint32_t)AAR_E_FAULT);
    CHECK(sha256_calls == 1U);

    /* decompress takes eleven words: algorithm, flags, src, a padding word,
     * src_bytes (stack words 0 and 1), dst, a padding word, dst_bytes
     * (stack words 4 and 5) and out. */
    memset(words, 0, sizeof(words));
    words[0] = AAR_COMPRESS_ZLIB;
    words[1] = 0x55U;
    words[2] = user_va + 0x100U;
    words[3] = 0xdeadbeefU;
    words[4] = 16U;
    words[6] = user_va + 0x200U;
    words[7] = 0xdeadbeefU;
    words[8] = 32U;
    words[10] = user_va + 0x300U;
    memset(user_page + 0x200U, 0xcc, 48U);
    memset(user_page + 0x300U, 0xcc, 32U);
    dispatch_words(api_slot(offsetof(aar_api, decompress)), &frame, words, 11U);
    CHECK(frame.r[0] == (uint32_t)AAR_OK);
    CHECK(decompress_calls == 1U);
    CHECK(decompress_algorithm_seen == AAR_COMPRESS_ZLIB);
    CHECK(decompress_flags_seen == 0x55U);
    CHECK(decompress_src_bytes_seen == 16U);
    CHECK(decompress_dst_bytes_seen == 32U);
    CHECK(decompress_src_seen == (const void *)(uintptr_t)(user_va + 0x100U));
    CHECK(decompress_dst_seen == (void *)(uintptr_t)(user_va + 0x200U));
    /* The destination is written by the handler in place, never copied; the
     * small result structure is copied out at its exact size. */
    CHECK(user_page[0x200U] == 0xccU);
    CHECK(user_page[0x317U] == 0U && user_page[0x318U] == 0xccU);

    /* Either buffer length above 32 bits: AAR_E_INVAL, call not made. */
    words[5] = 1U;
    dispatch_words(api_slot(offsetof(aar_api, decompress)), &frame, words, 11U);
    CHECK(frame.r[0] == (uint32_t)AAR_E_INVAL);
    words[5] = 0U;
    words[9] = 1U;
    dispatch_words(api_slot(offsetof(aar_api, decompress)), &frame, words, 11U);
    CHECK(frame.r[0] == (uint32_t)AAR_E_INVAL);
    CHECK(decompress_calls == 1U);
    /* The destination must be writable for its whole length. */
    words[9] = 0U;
    words[8] = 0x10000U;
    dispatch_words(api_slot(offsetof(aar_api, decompress)), &frame, words, 11U);
    CHECK(frame.r[0] == (uint32_t)AAR_E_FAULT);
    CHECK(decompress_calls == 1U);
    teardown_dispatch();
}

static void test_file_seek_offset_is_an_aligned_word_pair(void)
{
    zzapp_svc_frame_t frame;
    uint32_t words[6] = { 3U, 0xdeadbeefU, 0x10U, 2U, AAR_SEEK_END,
                          0U };

    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    memset(user_page + 0x400U, 0xcc, 16U);
    words[5] = user_va + 0x400U;
    dispatch_words(api_slot(offsetof(aar_api, file_seek)), &frame, words, 6U);
    CHECK(frame.r[0] == (uint32_t)AAR_OK);
    CHECK(file_seek_offset_seen == INT64_C(0x200000010));
    CHECK(file_seek_whence_seen == AAR_SEEK_END);
    /* out_pos is 8 bytes; the 8 after it are the caller's. */
    CHECK(user_page[0x407U] == 0U && user_page[0x408U] == 0xccU);

    words[2] = 0xfffffff0U;
    words[3] = 0xffffffffU;
    words[4] = AAR_SEEK_CUR;
    words[5] = 0U;
    dispatch_words(api_slot(offsetof(aar_api, file_seek)), &frame, words, 6U);
    CHECK(file_seek_offset_seen == -16);
    CHECK(file_seek_whence_seen == AAR_SEEK_CUR);
    teardown_dispatch();
}

static void test_size_first_info_extent(void)
{
    zzapp_svc_frame_t frame;
    uint32_t size;

    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    /* The caller's size is the extent: a smaller one copies less, a larger
     * one is zero-filled past what the runtime knows. */
    size = 64U;
    memset(user_page + 0x400U, 0xcc, 160U);
    memcpy(user_page + 0x400U, &size, sizeof(size));
    dispatch(api_slot(offsetof(aar_api, info)), &frame,
             (uint32_t[9]){ user_va + 0x400U });
    CHECK(frame.r[0] == (uint32_t)AAR_OK);
    CHECK(info_calls == 1U);
    CHECK(user_page[0x43fU] == 0xccU && user_page[0x440U] == 0xccU);

    size = 100U;
    memset(user_page + 0x400U, 0xcc, 160U);
    memcpy(user_page + 0x400U, &size, sizeof(size));
    dispatch(api_slot(offsetof(aar_api, info)), &frame,
             (uint32_t[9]){ user_va + 0x400U });
    CHECK(frame.r[0] == (uint32_t)AAR_OK);
    CHECK(user_page[0x400U + sizeof(aar_info) - 1U] == 0xccU);
    CHECK(user_page[0x400U + sizeof(aar_info)] == 0U &&
          user_page[0x400U + 99U] == 0U);
    CHECK(user_page[0x400U + 100U] == 0xccU);

    /* A size that runs off the mapped page is a bad pointer. */
    size = 0x1000U;
    memcpy(user_page + 0x400U, &size, sizeof(size));
    dispatch(api_slot(offsetof(aar_api, info)), &frame,
             (uint32_t[9]){ user_va + 0x400U });
    CHECK(frame.r[0] == (uint32_t)AAR_E_FAULT);
    CHECK(info_calls == 2U);
    teardown_dispatch();
}

#if ZZAPP_PERF_COUNTERS
static void test_gl_telemetry_snapshot_and_rate_limit(void)
{
    zzapp_svc_frame_t frame;
    zzapp_gl_telemetry_snapshot_t snapshot;
    uint32_t begin = ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, Begin));
    uint32_t end = ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, End));
    uint32_t vertex = ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, Vertex2f));
    uint32_t i;

    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    zzapp_svc_gl_telemetry_reset(0U);
    dispatch(api_slot(offsetof(aar_api, gl_bind)), &frame, (uint32_t[9]){ 1U });
    for (i = 0U; i < 63U; ++i)
        dispatch(begin, &frame, (uint32_t[9]){ 4U });
    CHECK(zzapp_svc_gl_telemetry_swap(0U, 0U, &snapshot) == 0);
    CHECK(snapshot.frames == 0U);
    CHECK(snapshot.gl_calls == 0U);

    for (i = 0U; i < 5U; ++i)
        dispatch(begin, &frame, (uint32_t[9]){ 4U });
    for (i = 0U; i < 3U; ++i)
        dispatch(end, &frame, (uint32_t[9]){ 0U });
    for (i = 0U; i < 3U; ++i)
        dispatch(vertex, &frame, (uint32_t[9]){ 0U });
    CHECK(zzapp_svc_gl_telemetry_swap(0U, 999999U, &snapshot) == 0);
    CHECK(snapshot.frames == 1U);
    CHECK(snapshot.gl_calls == 11U);
    CHECK(snapshot.gl_calls_per_frame == 11U);
    CHECK(snapshot.max_gl_calls == 11U);
    CHECK(snapshot.average_descriptor_cycles != 0U);
    CHECK(snapshot.average_frontend_cycles != 0U);
    CHECK(snapshot.top_slots[0] == gl_slot(offsetof(aar_gl, Begin)));
    CHECK(snapshot.top_counts[0] == 5U);
    CHECK(snapshot.top_slots[1] == gl_slot(offsetof(aar_gl, End)));
    CHECK(snapshot.top_counts[1] == 3U);
    CHECK(snapshot.top_slots[2] == gl_slot(offsetof(aar_gl, Vertex2f)));
    CHECK(snapshot.top_counts[2] == 3U);
    CHECK(snapshot.top_counts[3] == 0U);
    CHECK(snapshot.top_slots[3] != UINT32_MAX);


    dispatch(begin, &frame, (uint32_t[9]){ 4U });
    CHECK(zzapp_svc_gl_telemetry_swap(0U, 1000000U, &snapshot) == 1);
    CHECK(snapshot.frames == 2U);
    CHECK(snapshot.gl_calls == 12U);
    CHECK(snapshot.gl_calls_per_frame == 6U);
    CHECK(snapshot.max_gl_calls == 11U);
    CHECK(zzapp_svc_gl_telemetry_swap(0U, 1000001U, &snapshot) == 0);
    CHECK(snapshot.frames == 1U);
    CHECK(snapshot.gl_calls == 0U);
    teardown_dispatch();
}
#endif

#define SVC_RECORDS 8U

/* The state the SVC layer keeps per app lives in the app's record: its two
 * table pages, its veneer and table aliases, its GL client and telemetry,
 * its counters and its last SVC number. Eight records, none shared. */
static void test_user_tables_and_state_are_per_record(void)
{
    zzapp_svc_frame_t frame;
    const aar_api *tables[SVC_RECORDS];
    uint8_t before[sizeof(aar_api)];
    uint32_t file_seek_slot = api_slot(offsetof(aar_api, file_seek));
    uint32_t pid;
    uint32_t other;

    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    CHECK(zzapp_rec_capacity() == SVC_RECORDS);
    for (pid = 0U; pid < SVC_RECORDS; ++pid) {
        CHECK(zzapp_svc_set_veneer_alias(pid, 0xd0000000U + pid * 0x10000U) ==
              AAR_OK);
        CHECK(zzapp_svc_set_table_aliases(pid, 0xd8000000U + pid * 0x10000U,
                                          0xd8001000U + pid * 0x10000U) == AAR_OK);
        tables[pid] = zzapp_svc_api_for_slot(pid, &api);
        CHECK(tables[pid] == (const aar_api *)zzapp_rec_tables(pid));
        CHECK(((uintptr_t)tables[pid] & 0xfffU) == 0U);
        CHECK((void *)zzapp_svc_gl_for_slot(pid) ==
              (void *)((uint8_t *)zzapp_rec_tables(pid) +
                       ZZAPP_REC_GL_PAGE_OFFSET));
    }
    /* Distinct pages: two per record, so at least 8 KiB apart. */
    for (pid = 0U; pid < SVC_RECORDS; ++pid)
        for (other = pid + 1U; other < SVC_RECORDS; ++other) {
            uintptr_t gap = (uintptr_t)tables[other] > (uintptr_t)tables[pid] ?
                (uintptr_t)tables[other] - (uintptr_t)tables[pid] :
                (uintptr_t)tables[pid] - (uintptr_t)tables[other];

            CHECK(gap >= 0x2000U);
        }
    /* Each table carries its own record's veneer base and build id. */
    for (pid = 0U; pid < SVC_RECORDS; ++pid) {
        const uintptr_t *entries = (const uintptr_t *)(const void *)
            ((const uint8_t *)tables[pid] + offsetof(aar_api, exit));
        const aar_ext_zz9000 *ext = zzapp_svc_ext_for_slot(pid);

        CHECK(entries[file_seek_slot] ==
              0xd0000000U + pid * 0x10000U + file_seek_slot * 12U);
        CHECK(ext != NULL);
        if (ext)
            CHECK((uintptr_t)ext->build_id >= 0xd8000000U + pid * 0x10000U &&
                  (uintptr_t)ext->build_id < 0xd8001000U + pid * 0x10000U);
    }
    /* Filling one record's page touches no other. */
    memcpy(before, tables[5], sizeof(before));
    CHECK(zzapp_svc_set_veneer_alias(3U, 0xe0000000U) == AAR_OK);
    CHECK(zzapp_svc_api_for_slot(3U, &api) == tables[3]);
    CHECK(memcmp(before, tables[5], sizeof(before)) == 0);
    CHECK(((const uintptr_t *)(const void *)((const uint8_t *)tables[3] +
           offsetof(aar_api, exit)))[file_seek_slot] ==
          0xe0000000U + file_seek_slot * 12U);
    CHECK(((const uintptr_t *)(const void *)((const uint8_t *)tables[4] +
           offsetof(aar_api, exit)))[file_seek_slot] ==
          0xd0000000U + 4U * 0x10000U + file_seek_slot * 12U);

    /* A pid outside the record table fails; it does not fall back to 0. */
    CHECK(zzapp_svc_api_for_slot(SVC_RECORDS, &api) == NULL);
    CHECK(zzapp_svc_api_for_slot(ZZAPP_PID_NONE, &api) == NULL);
    CHECK(zzapp_svc_gl_for_slot(SVC_RECORDS) == NULL);
    CHECK(zzapp_svc_ext_for_slot(SVC_RECORDS) == NULL);
    CHECK(zzapp_svc_set_veneer_alias(SVC_RECORDS, 0xd0000000U) == AAR_E_INVAL);
    CHECK(zzapp_svc_set_table_aliases(SVC_RECORDS, 0xd0001000U, 0xd0002000U) ==
          AAR_E_INVAL);
    CHECK(zzapp_svc_last_number_for(SVC_RECORDS) == 0U);

    /* Last SVC numbers are per record: fast calls count for the thread. */
    zzapp_kernel_set_current_thread(5);
    dispatch(api_slot(offsetof(aar_api, should_stop)), &frame,
             (uint32_t[9]){ 0U });
    CHECK(zzapp_svc_last_number_for(5U) == api_slot(offsetof(aar_api, should_stop)));
    CHECK(zzapp_svc_last_number_for(0U) == 0U);
    CHECK(zzapp_svc_last_number_for(4U) == 0U);
    /* A call with no thread and no services record is refused, not
     * charged to record 0. */
    zzapp_kernel_set_current_thread(-1);
    services_pid = ZZAPP_PID_NONE;
    dispatch(api_slot(offsetof(aar_api, should_stop)), &frame,
             (uint32_t[9]){ 0U });
    CHECK(frame.r[0] == (uint32_t)AAR_E_FAULT);
    CHECK(zzapp_svc_last_number_for(0U) == 0U);
    services_pid = 0U;

#if ZZAPP_PERF_COUNTERS
    {
        uint32_t total = 0U;
        uint32_t samples[4];
        uint32_t averages[4];
        zzapp_gl_telemetry_snapshot_t snap3;
        zzapp_gl_telemetry_snapshot_t snap5;
        uint32_t begin = ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, Begin));

        /* Counters: the vector's entry stamp goes through the published
         * pointer, the C side reads it from the same record. */
        zzapp_svc_counters_reset(5U);
        zzapp_kernel_set_current_thread(5);
        svc_counter_now = 0U;
        asm_entry_stamp(0U);
        dispatch(api_slot(offsetof(aar_api, should_stop)), &frame,
                 (uint32_t[9]){ 0U });
        zzapp_svc_counters_test_snapshot(5U, &total, samples, averages);
        CHECK(total == 1U);
        zzapp_svc_counters_test_snapshot(0U, &total, samples, averages);
        CHECK(total == 0U);
        zzapp_svc_counters_test_snapshot(4U, &total, samples, averages);
        CHECK(total == 0U);

        /* GL: bind under record 3, count two calls; record 5 saw none. */
        zzapp_kernel_set_current_thread(-1);
        services_pid = 3U;
        zzapp_svc_gl_telemetry_reset(3U);
        zzapp_svc_gl_telemetry_reset(5U);
        dispatch(api_slot(offsetof(aar_api, gl_bind)), &frame, (uint32_t[9]){ 1U });
        CHECK(frame.r[0] == 0xd8001000U + 3U * 0x10000U);
        CHECK(zzapp_svc_gl_telemetry_swap(3U, 0U, &snap3) == 0);
        CHECK(zzapp_svc_gl_telemetry_swap(5U, 0U, &snap5) == 0);
        dispatch(begin, &frame, (uint32_t[9]){ 4U });
        dispatch(begin, &frame, (uint32_t[9]){ 4U });
        CHECK(zzapp_svc_gl_telemetry_swap(3U, 1U, &snap3) == 0);
        CHECK(zzapp_svc_gl_telemetry_swap(5U, 1U, &snap5) == 0);
        CHECK(snap3.gl_calls == 2U);
        CHECK(snap5.gl_calls == 0U);
        /* Only record 3 has a GL table; record 5 never bound. */
        CHECK(zzapp_svc_gl_for_slot(3U)->size != 0U);
        CHECK(zzapp_svc_gl_for_slot(5U)->size == 0U);
        services_pid = 0U;
    }
#endif
    zzapp_kernel_set_current_thread(0);
    teardown_dispatch();
}

/* ---- the scalar GL fast path ------------------------------------------- */

static uint32_t fast_enable_arg;
static unsigned fast_enable_calls;
static uint32_t fast_bind_args[2];
static uint32_t fast_viewport_args[4];
static uint32_t fast_copy_args[8];
static uint32_t fast_vertex3f_words[3];

static void mock_fast_enable(uint32_t cap)
{
    fast_enable_arg = cap;
    ++fast_enable_calls;
}
static void mock_fast_vertex3f(float x, float y, float z)
{
    memcpy(&fast_vertex3f_words[0], &x, sizeof(x));
    memcpy(&fast_vertex3f_words[1], &y, sizeof(y));
    memcpy(&fast_vertex3f_words[2], &z, sizeof(z));
}
static void mock_fast_bind(uint32_t target, uint32_t texture)
{
    fast_bind_args[0] = target;
    fast_bind_args[1] = texture;
}
static void mock_fast_viewport(int32_t x, int32_t y, int32_t w, int32_t h)
{
    fast_viewport_args[0] = (uint32_t)x;
    fast_viewport_args[1] = (uint32_t)y;
    fast_viewport_args[2] = (uint32_t)w;
    fast_viewport_args[3] = (uint32_t)h;
}
/* Eight words: four in registers, four on the user's stack. Written as the
 * dispatcher hands words over on this 64-bit host (see the note above
 * mock_file_seek_words). */
static void mock_fast_copy_words(uint32_t w0, uint32_t w1, uint32_t w2,
                                 uint32_t w3, uint32_t w4, uint32_t w5,
                                 uint32_t w6, uint32_t w7, uint32_t w8,
                                 uint32_t w9, uint32_t w10, uint32_t w11)
{
    (void)w8; (void)w9; (void)w10; (void)w11;
    fast_copy_args[0] = w0; fast_copy_args[1] = w1;
    fast_copy_args[2] = w2; fast_copy_args[3] = w3;
    fast_copy_args[4] = w4; fast_copy_args[5] = w5;
    fast_copy_args[6] = w6; fast_copy_args[7] = w7;
}

static void fast_state_clear(void)
{
    fast_enable_arg = 0U;
    fast_enable_calls = 0U;
    memset(fast_bind_args, 0, sizeof(fast_bind_args));
    memset(fast_viewport_args, 0, sizeof(fast_viewport_args));
    memset(fast_copy_args, 0, sizeof(fast_copy_args));
    memset(fast_vertex3f_words, 0, sizeof(fast_vertex3f_words));
}

static void test_scalar_gl_fast_path(void)
{
    zzapp_svc_frame_t frame;
    const uint32_t begin = ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, Begin));

    setup_dispatch();
    fast_state_clear();
    gl.Enable = mock_fast_enable;
    gl.BindTexture = mock_fast_bind;
    gl.Viewport = mock_fast_viewport;
    gl.CopyTexSubImage2D = (void (*)(uint32_t, int32_t, int32_t, int32_t,
                                     int32_t, int32_t, int32_t, int32_t))(void *)
                        mock_fast_copy_words;
    dispatch(api_slot(offsetof(aar_api, gl_bind)), &frame, (uint32_t[9]){ 1U });
    CHECK(frame.r[0] != 0U);
    zzapp_kernel_set_current_thread(0);
    /* A scalar call with one word: the module entry sees it. */
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, Enable)),
             &frame, (uint32_t[9]){ 0x0b71U });
    CHECK(fast_enable_calls == 1U && fast_enable_arg == 0x0b71U);
    CHECK(frame.r[0] != (uint32_t)AAR_E_FAULT && frame.r[0] != (uint32_t)AAR_E_NOSYS);
    /* Two and four words. */
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, BindTexture)),
             &frame, (uint32_t[9]){ 0x0de1U, 7U });
    CHECK(fast_bind_args[0] == 0x0de1U && fast_bind_args[1] == 7U);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, Viewport)),
             &frame, (uint32_t[9]){ 1U, 2U, 3U, 4U });
    CHECK(fast_viewport_args[0] == 1U && fast_viewport_args[1] == 2U &&
          fast_viewport_args[2] == 3U && fast_viewport_args[3] == 4U);
    /* Eight words: the last four come from the checked user stack. */
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, CopyTexSubImage2D)),
             &frame, (uint32_t[9]){ 11U, 12U, 13U, 14U, 15U, 16U, 17U, 18U });
    CHECK(fast_copy_args[0] == 11U && fast_copy_args[3] == 14U &&
          fast_copy_args[4] == 15U && fast_copy_args[7] == 18U);
    /* A float call keeps its arguments: the frame's saved s0-s2 go to the
     * entry as floats (on the card the registers themselves are passed). */
    gl.Vertex3f = mock_fast_vertex3f;
    memset(&frame, 0, sizeof(frame));
    frame.r[12] = ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, Vertex3f));
    frame.vfp[0] = 0x3f800000U; /* 1.0f */
    frame.vfp[1] = 0x40000000U; /* 2.0f */
    frame.vfp[2] = 0x40400000U; /* 3.0f */
    frame.vfp[3] = 0x40800000U; /* 4.0f, unused by Vertex3f */
    zzapp_svc_dispatch(&frame);
    CHECK(fast_vertex3f_words[0] == 0x3f800000U &&
          fast_vertex3f_words[1] == 0x40000000U &&
          fast_vertex3f_words[2] == 0x40400000U);
    /* The stop report's veneer number is kept. */
    CHECK(zzapp_svc_last_number_for(0U) ==
          ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, Vertex3f)));
    /* The client array state records the call (Begin/End, as the checked
     * path does; the array rules are covered by the span tests above). */
    dispatch(begin, &frame, (uint32_t[9]){ 4U });
    CHECK(gl_begin_calls == 1U);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, End)),
             &frame, (uint32_t[9]){ 0U });
    CHECK(gl_end_calls == 1U);
    /* A pending GL error is delivered by the checked path, which owns it. */
    mock_gl_error = 0x0502U;
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, GetError)),
             &frame, (uint32_t[9]){ 0U });
    CHECK(frame.r[0] == 0x0502U);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, GetError)),
             &frame, (uint32_t[9]){ 0U });
    CHECK(frame.r[0] == 0U);
    /* A pointer call takes the checked path: the bad pointer is refused and
     * the module never sees it. */
    getter_calls = 0U;
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, GetIntegerv)),
             &frame, (uint32_t[9]){ 0x0ba2U, user_va + 0xffcU });
    CHECK(getter_calls == 0U);
    CHECK(frame.r[0] == (uint32_t)AAR_E_FAULT);
    /* An empty table slot is AAR_E_NOSYS, as on the checked path. */
    gl.LineWidth = NULL;
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, LineWidth)),
             &frame, (uint32_t[9]){ 0U });
    CHECK(frame.r[0] == (uint32_t)AAR_E_NOSYS);
    /* Not an app thread: no fast path. (With no record selected the checked
     * path refuses with AAR_E_FAULT, which test_core_aware... covers.) */
    zzapp_kernel_set_current_thread(-1);
    services_pid = ZZAPP_PID_NONE;
    fast_enable_calls = 0U;
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, Enable)),
             &frame, (uint32_t[9]){ 0x0b71U });
    CHECK(fast_enable_calls == 0U);
    CHECK(frame.r[0] == (uint32_t)AAR_E_FAULT);
    services_pid = 0U;
    zzapp_kernel_set_current_thread(0);
    teardown_dispatch();
}

/*
 * glVertex3fv through the SVC layer. The digest covers everything the test can
 * observe (status word, call count, the bits the module read, whether the
 * module saw a kernel copy) and equals the digest of the checked path alone.
 */
static uint32_t v3_digest = 2166136261U;
static unsigned v3_calls;
static uint32_t v3_bits[3];
static int v3_kernel_copy;
static uint32_t v3_user_lo, v3_user_hi;

static void v3_mix(uint32_t word)
{
    v3_digest = (v3_digest ^ word) * 16777619U;
}
static void v3_mock(const float *v)
{
    uintptr_t address = (uintptr_t)v;

    ++v3_calls;
    memcpy(v3_bits, v, sizeof(v3_bits));
    v3_kernel_copy = (address & 3U) == 0U &&
                     (address < v3_user_lo || address >= v3_user_hi);
}
static void v3_call(uint32_t address)
{
    zzapp_svc_frame_t frame;
    unsigned before = v3_calls;

    v3_bits[0] = v3_bits[1] = v3_bits[2] = 0xdeadbeefU;
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, Vertex3fv)),
             &frame, (uint32_t[9]){ address });
    v3_mix(frame.r[0]);
    v3_mix(v3_calls - before);
    if (v3_calls != before) {
        CHECK(v3_kernel_copy);
        v3_mix(v3_bits[0]);
        v3_mix(v3_bits[1]);
        v3_mix(v3_bits[2]);
    }
}
static void test_vertex3fv_body(void)
{
    static const uint32_t words[3] = { 0x80000000U, 0x3f800000U, 0xc0490fdbU };
    zzapp_svc_frame_t frame;
    uint32_t extra_va = 0U;
    unsigned before;

    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    gl.Vertex3fv = v3_mock;
    v3_user_lo = user_va;
    v3_user_hi = user_va + 0x3000U;
    v3_calls = 0U;
    /* No binding yet: nothing reaches the module, in both builds. */
    memcpy(user_page + 0x100U, words, sizeof(words));
    v3_call(user_va + 0x100U);
    CHECK(v3_calls == 0U);
    dispatch(api_slot(offsetof(aar_api, gl_bind)), &frame, (uint32_t[9]){ 1U });
    CHECK(frame.r[0] != 0U);

    /* Aligned, a signed zero first, outside Begin. */
    v3_call(user_va + 0x100U);
    CHECK(v3_calls == 1U && memcmp(v3_bits, words, sizeof(words)) == 0);
    /* Offsets that are not float aligned still read the right bytes. */
    for (uint32_t offset = 1U; offset < 4U; ++offset) {
        memcpy(user_page + 0x200U + offset, words, sizeof(words));
        before = v3_calls;
        v3_call(user_va + 0x200U + offset);
        CHECK(v3_calls == before + 1U && memcmp(v3_bits, words, sizeof(words)) == 0);
    }
    /* The pointer itself, not the extent, is bad. */
    before = v3_calls;
    v3_call(0U);
    v3_call(0xfffffff8U);
    v3_call(0x50000000U);
    v3_call(user_va + 0xff8U); /* runs off the mapping */
    CHECK(v3_calls == before);
    /* An extent that crosses into a second page: refused while the page is
     * unmapped, accepted when it is mapped read-only. */
    CHECK(zzapp_as_alias_map(&proc.as, 0x02001000U, 4096U, 0, &extra_va) == AAR_OK);
    if (extra_va == user_va + 0x1000U) {
        memcpy(user_page + 0xff8U, words, 8U);
        memcpy(user_page + 0x1000U, (const uint8_t *)words + 8U, 4U);
        before = v3_calls;
        v3_call(user_va + 0xff8U);
        CHECK(v3_calls == before + 1U && memcmp(v3_bits, words, sizeof(words)) == 0);
        v3_call(user_va + 0x1ffcU); /* tail beyond the read only page */
        CHECK(v3_calls == before + 1U);
    }
    CHECK(zzapp_as_alias_unmap(&proc.as, extra_va, 4096U) == AAR_OK);
    before = v3_calls;
    v3_call(user_va + 0xff8U);
    CHECK(v3_calls == before);

    /* A pending GL error stays for the next glGetError and does not change
     * what the module receives. */
    mock_gl_error = 0x0500U;
    v3_call(user_va + 0x100U);
    CHECK(v3_calls == before + 1U);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, GetError)),
             &frame, (uint32_t[9]){ 0U });
    v3_mix(frame.r[0]);
    CHECK(frame.r[0] == 0x0500U);

    /* Begin and End around it make no difference to the call. */
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, Begin)),
             &frame, (uint32_t[9]){ 4U });
    before = v3_calls;
    v3_call(user_va + 0x100U);
    CHECK(v3_calls == before + 1U);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(aar_gl, End)),
             &frame, (uint32_t[9]){ 0U });

    /* An empty table slot. */
    gl.Vertex3fv = NULL;
    before = v3_calls;
    v3_call(user_va + 0x100U);
    CHECK(v3_calls == before);
    gl.Vertex3fv = v3_mock;

    /* A stale generation and an ended binding. */
    proc.generation++;
    v3_call(user_va + 0x100U);
    CHECK(v3_calls == before);
    proc.generation--;
    v3_call(user_va + 0x100U);
    CHECK(v3_calls == before + 1U);
    dispatch(api_slot(offsetof(aar_api, gl_unbind)), &frame, (uint32_t[9]){ 0U });
    v3_call(user_va + 0x100U);
    CHECK(v3_calls == before + 1U);

    /* Not an app thread. */
    dispatch(api_slot(offsetof(aar_api, gl_bind)), &frame, (uint32_t[9]){ 1U });
    zzapp_kernel_set_current_thread(-1);
    services_pid = ZZAPP_PID_NONE;
    v3_call(user_va + 0x100U);
    CHECK(v3_calls == before + 1U);
    services_pid = 0U;
    zzapp_kernel_set_current_thread(0);
    /* The digest was recorded from the checked path before the fast route
     * existed, so it pins the fast route to the old behaviour. */
    CHECK(v3_digest == 0x8aedd21aU && v3_calls == 8U);
    teardown_dispatch();
}

/* The host build is 64-bit, but the dispatcher hands kernel buffers to
 * handlers as 32-bit words. Tests that dereference one need a low stack. */
static ucontext_t caller_context, body_context;
static void run_on_low_stack(void (*body)(void))
{
    const size_t bytes = 1U << 19;
    void *stack = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);

    CHECK(stack != MAP_FAILED);
    if (stack == MAP_FAILED)
        return;
    CHECK(getcontext(&body_context) == 0);
    body_context.uc_stack.ss_sp = stack;
    body_context.uc_stack.ss_size = bytes;
    body_context.uc_link = &caller_context;
    makecontext(&body_context, body, 0);
    CHECK(swapcontext(&caller_context, &body_context) == 0);
    munmap(stack, bytes);
}

int main(void)
{
    test_scalar_gl_fast_path();
    run_on_low_stack(test_vertex3fv_body);
    test_table_copy_uses_only_veneers();
    test_gl_table_copy_preserves_missing_calls();
    test_pointer_checks();
    test_file_request_pointers();
    run_on_low_stack(test_requester_pointers);
    test_large_and_section_pointer_checks();
    test_gl_span_validation();
    test_gl_bad_array_pointer_fails_at_draw();
    test_gl_span_fallback_reference();
    test_gl_lock_calls_stay_kernel_side();
    test_descriptors_cover_callable_slots();
    test_gl_deferred_pointer_checks();
    test_copy_out_and_pixel_argument_positions();
    test_descriptor_shapes();
    test_extended_pointer_checks();
#if ZZAPP_PERF_COUNTERS
    test_sampled_counter_averages();
#endif
    test_svc_uses_core_aware_slot_resolver();
    test_gl_getters_copy_full_outputs();
    test_gl_strings_survive_other_queries();
    test_minor6_gl_pointer_extents();
    test_clip_plane_copies_four_doubles();
    test_minor6_present_timestamp_uses_both_registers();
    test_present_validates_full_outputs();
    test_svc_number_comes_from_r12();
    test_fast_no_lock_calls();
#if ZZAPP_SVC_PROBES
    test_svc_probes_do_not_run_real_handlers();
#endif
#if ZZAPP_SVC_PROBES && ZZAPP_PERF_COUNTERS
    test_svc_probe_shallow_exits_clear_counter_tail();
#endif
    test_svc_float_frame_arguments();
    test_rejected_pixel_store_does_not_change_validation();
#if ZZAPP_PERF_COUNTERS
    test_gl_telemetry_snapshot_and_rate_limit();
#endif
    test_ext_table_for_the_app();
    test_user_tables_and_state_are_per_record();
    test_ext_call_and_gl_bind_flags();
    test_service_lengths_are_word_pairs();
    test_file_seek_offset_is_an_aligned_word_pair();
    test_size_first_info_extent();
    /* Last: it installs slot aliases that stay for the rest of the process. */
    test_slot_aliases();
    printf("svc_test: %u checks, %d failures\n", checks, failures);
    return failures != 0;
}
