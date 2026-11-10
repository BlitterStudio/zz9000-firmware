/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "zzapp.h"
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
static void setup(zzapp_as_t *as)
{
    memset(bsp, 0, sizeof(bsp));
    bsp[0] = SECTION;
    bsp[0x20U] = SECTION;
    zzapp_as_init(bsp);
    CHECK(zzapp_as_create(as, 0U, 1U) == 0);
    CHECK(zzapp_as_map_image(as, WINDOW, 0x01000000U, 4096U,
                             ZZAPP_AS_IMAGE_RW) == 0);
    CHECK(zzapp_as_map_image(as, WINDOW + 0x1000U, 0x01001000U, 4096U,
                             ZZAPP_AS_IMAGE_RO) == 0);
    CHECK(zzapp_as_alias_map(as, 0x02000000U, 4096U, 1, &user_va) == 0);
}
static zzapp_proc_t proc;
static zzapp_api api;
static zzapp_gl gl;
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
static int svc_test_current_thread;
static unsigned should_stop_calls;
static unsigned time_us_calls;
static uint32_t vertex2f_words[2];
#if ZZAPP_PERF_COUNTERS
static uint32_t svc_counter_now;

uint32_t zzapp_svc_test_pmccntr(void)
{
    svc_counter_now += 10U;
    return svc_counter_now;
}

void zzapp_svc_counters_test_eret(uint32_t slot, uint32_t stamp);
void zzapp_svc_counters_test_snapshot(uint32_t slot, uint32_t *total_calls,
                                      uint32_t samples[4], uint32_t averages[4]);
#endif

int zzapp_kernel_current_thread(void) { return svc_test_current_thread; }
zzapp_proc_t *zzapp_proc_slot(uint32_t slot)
{
    return slot == 0U ? &proc : NULL;
}
const zzapp_api *zzapp_api_table(void) { return &api; }
uint32_t zzapp_services_current_slot(void) { return 0U; }

static const zzapp_gl *mock_gl_bind(uint32_t surface_id)
{
    return surface_id ? &gl : NULL;
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
static int mock_file_list_next(int list, zzapp_dirent *entry)
{
    (void)list;
    ++file_list_next_calls;
    if (file_list_next_calls == 1U) {
        memset(entry, 0x31, sizeof(*entry));
        return ZZ_OK;
    }
    return ZZ_EINVAL;
}
static int mock_heap_shrink(uint32_t bytes, void **new_break)
{
    (void)bytes;
    (void)new_break;
    ++heap_shrink_calls;
    return ZZ_OK;
}
static uint32_t mock_get_error(void) { return 0U; }
static void mock_begin(uint32_t mode)
{
    (void)mode;
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
static int mock_file_seek(int fd, int64_t offset, int whence, int64_t *out)
{
    (void)fd;
    (void)offset;
    (void)whence;
    ++file_seek_calls;
    if (out)
        *out = 0;
    return ZZ_OK;
}
static void mock_heap_range(void **base, uint32_t *bytes)
{
    ++heap_range_calls;
    if (base)
        *base = NULL;
    if (bytes)
        *bytes = 0U;
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

static uint32_t gl_slot(size_t field)
{
    return (uint32_t)((field - offsetof(zzapp_gl, Begin)) / sizeof(void *));
}
static uint32_t api_slot(size_t field)
{
    return (uint32_t)((field - offsetof(zzapp_api, exit)) / sizeof(void *));
}
static void dispatch(uint32_t number, zzapp_svc_frame_t *frame,
                     const uint32_t args[9])
{
    uint32_t i;

    memset(frame, 0, sizeof(*frame));
    *(uint32_t *)(void *)user_page = 0xef000000U;
    frame->exception_lr = user_va + 4U;
    frame->r[12] = number;
    for (i = 0U; i < 4U; ++i)
        frame->r[i] = args[i];
    frame->user_sp = user_va + 0x800U;
    for (; i < 9U; ++i)
        *(uint32_t *)(void *)(user_page + 0x800U + (i - 4U) * 4U) = args[i];
    zzapp_svc_dispatch(frame);
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
    api.file_seek = mock_file_seek;
    api.should_stop = mock_should_stop;
    api.time_us = mock_time_us;
    api.heap_range = mock_heap_range;
    api.msg_poll = mock_msg_poll;
    gl.Begin = mock_begin;
    gl.End = mock_end;
    gl.GetIntegerv = mock_get_integerv;
    gl.GetFloatv = (void (*)(uint32_t, float *))mock_get_integerv;
    gl.Vertex2f = mock_vertex2f;
    gl.VertexPointer = mock_vertex_pointer;
    gl.EnableClientState = mock_enable_client_state;
    gl.DrawArrays = mock_draw_arrays;
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

static void test_table_copy_uses_only_veneers(void)
{
    zzapp_api direct;
    zzapp_api app;
    uintptr_t *entries;
    uint32_t i;

    memset(&direct, 0, sizeof(direct));
    direct.size = sizeof(direct);
    direct.version = ZZAPP_API_VERSION;
    direct.file_read = (void *)(uintptr_t)0x12345678U;
    direct.time_us = (void *)(uintptr_t)0x12345678U;
    direct.build_id = "build";
    CHECK(zzapp_svc_copy_api(&app, &direct) == ZZ_OK);
    CHECK(app.size == direct.size);
    CHECK(app.version == direct.version);
    CHECK(app.build_id != direct.build_id);
    entries = (uintptr_t *)(void *)((uint8_t *)&app + offsetof(zzapp_api, exit));
    for (i = 0; i < zzapp_svc_api_count(); ++i)
        if (i != (offsetof(zzapp_api, build_id) -
                  offsetof(zzapp_api, exit)) / sizeof(void *))
            CHECK(zzapp_svc_is_veneer(entries[i]));
}

static void test_gl_table_copy_uses_only_veneers(void)
{
    zzapp_gl direct;
    zzapp_gl app;
    uintptr_t *entries;
    uint32_t i;

    memset(&direct, 0, sizeof(direct));
    direct.size = sizeof(direct);
    direct.Begin = (void *)(uintptr_t)0x12345678U;
    direct.build_id = "gl-build";
    CHECK(zzapp_svc_copy_gl(&app, &direct) == ZZ_OK);
    CHECK(app.size == direct.size);
    CHECK(app.build_id != direct.build_id);
    entries = (uintptr_t *)(void *)((uint8_t *)&app + offsetof(zzapp_gl, Begin));
    for (i = 0; i < zzapp_svc_gl_count(); ++i)
        if (i != (offsetof(zzapp_gl, build_id) -
                  offsetof(zzapp_gl, Begin)) / sizeof(void *))
            CHECK(zzapp_svc_is_veneer(entries[i]));
}

static void test_descriptors_cover_callable_slots(void)
{
    uint32_t i;
    uint32_t api_build = (uint32_t)((offsetof(zzapp_api, build_id) -
                                     offsetof(zzapp_api, exit)) /
                                    sizeof(void *));
    uint32_t gl_build = (uint32_t)((offsetof(zzapp_gl, build_id) -
                                    offsetof(zzapp_gl, Begin)) /
                                   sizeof(void *));

    for (i = 0U; i < zzapp_svc_api_count(); ++i)
        if (i != api_build)
            CHECK(zzapp_svc_api_descriptor(i) != NULL);
    for (i = 0U; i < zzapp_svc_gl_count(); ++i)
        if (i != gl_build)
            CHECK(zzapp_svc_gl_descriptor(i) != NULL);
}

static void test_descriptor_shapes(void)
{
    const zzapp_svc_descriptor_t *api;
    const zzapp_svc_descriptor_t *gl;

    api = zzapp_svc_api_descriptor((uint32_t)((offsetof(zzapp_api, file_open) -
                                               offsetof(zzapp_api, exit)) /
                                              sizeof(void *)));
    CHECK(api->arguments == 2U);
    CHECK(api->pointers[0].direction == ZZAPP_SVC_POINTER_IN);
    CHECK(api->pointers[0].length == ZZAPP_SVC_LENGTH_STRING);
    api = zzapp_svc_api_descriptor((uint32_t)((offsetof(zzapp_api, display_open) -
                                               offsetof(zzapp_api, exit)) /
                                              sizeof(void *)));
    CHECK(api->arguments == 5U);
    CHECK(api->pointers[0].direction == ZZAPP_SVC_POINTER_OUT);
    CHECK(api->pointers[0].element_bytes == sizeof(zzapp_surface));
    api = zzapp_svc_api_descriptor((uint32_t)((offsetof(zzapp_api, input_poll) -
                                               offsetof(zzapp_api, exit)) /
                                              sizeof(void *)));
    CHECK(api->pointers[0].length == ZZAPP_SVC_LENGTH_COUNT);
    CHECK(api->pointers[0].element_bytes == sizeof(zzapp_event));
    api = zzapp_svc_api_descriptor((uint32_t)((offsetof(zzapp_api, time_us) -
                                               offsetof(zzapp_api, exit)) /
                                              sizeof(void *)));
    CHECK(api->result == ZZAPP_SVC_RETURN_U64);
    api = zzapp_svc_api_descriptor((uint32_t)((offsetof(zzapp_api, yield) -
                                               offsetof(zzapp_api, exit)) /
                                              sizeof(void *)));
    CHECK(api->result == ZZAPP_SVC_RETURN_VOID);
    gl = zzapp_svc_gl_descriptor((uint32_t)((offsetof(zzapp_gl, Vertex2f) -
                                             offsetof(zzapp_gl, Begin)) /
                                            sizeof(void *)));
    CHECK(gl->arguments == 0U);
    CHECK(gl->vfp_words == 2U);
    api = zzapp_svc_api_descriptor((uint32_t)((offsetof(zzapp_api, should_stop) -
                                               offsetof(zzapp_api, exit)) /
                                              sizeof(void *)));
    CHECK(api->flags == ZZAPP_SVC_FAST_NO_LOCK);
    api = zzapp_svc_api_descriptor((uint32_t)((offsetof(zzapp_api, time_us) -
                                               offsetof(zzapp_api, exit)) /
                                              sizeof(void *)));
    CHECK(api->flags == ZZAPP_SVC_FAST_NO_LOCK);
    api = zzapp_svc_api_descriptor((uint32_t)((offsetof(zzapp_api, mem_info) -
                                               offsetof(zzapp_api, exit)) /
                                              sizeof(void *)));
    CHECK(api->flags == 0U);
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
    CHECK(zzapp_svc_check_user(&as, WINDOW, 4096U, 1) == ZZ_OK);
    CHECK(zzapp_svc_check_user(&as, WINDOW + 4095U, 2U, 1) == ZZ_EFAULT);
    CHECK(zzapp_svc_check_user(&as, WINDOW + 0x1000U, 4U, 1) == ZZ_EFAULT);
    CHECK(zzapp_svc_check_user(&as, WINDOW + 0x1000U, 4U, 0) == ZZ_OK);
    CHECK(zzapp_svc_check_user(&as, user_va, 4U, 1) == ZZ_OK);
    CHECK(zzapp_svc_check_user(&as, 0x02000000U, 4U, 1) == ZZ_EFAULT);
    CHECK(zzapp_svc_check_user(&as, 0xfffffff0U, 32U, 0) == ZZ_EFAULT);
    zzapp_as_destroy(&as);
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
    dispatch(api_slot(offsetof(zzapp_api, gl_bind)), &frame, (uint32_t[9]){ 1U });
    CHECK(frame.r[0] != 0U);
#if ZZAPP_PERF_COUNTERS
    CHECK(zzapp_svc_gl_telemetry_swap(0U, 0U, NULL) == 0);
#endif

    CHECK(zzapp_as_alias_map(&proc.as, 0x02001000U, 4096U, 1,
                             &extra_va) == ZZ_OK);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(zzapp_gl, VertexPointer)),
             &frame, (uint32_t[9]){ 3U, 0x1406U, 0U, user_va + 0x100U });
    CHECK(vertex_pointer_calls == 1U);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(zzapp_gl, EnableClientState)),
             &frame, (uint32_t[9]){ 0x8074U });
    CHECK(zzapp_as_alias_unmap(&proc.as, extra_va, 4096U) == ZZ_OK);
    args[0] = 4U;
    args[1] = 0U;
    args[2] = 1024U;
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(zzapp_gl, DrawArrays)),
             &frame, args);
    CHECK(draw_arrays_calls == 0U);
    CHECK(frame.r[0] == (uint32_t)ZZ_EFAULT);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(zzapp_gl, GetError)),
             &frame, (uint32_t[9]){ 0U });
    CHECK(frame.r[0] == 0x0501U);
    CHECK(zzapp_as_alias_map(&proc.as, 0x02001000U, 12288U, 1,
                             &extra_va) == ZZ_OK);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(zzapp_gl, DrawArrays)),
             &frame, args);
    CHECK(draw_arrays_calls == 1U);
    CHECK(zzapp_as_alias_unmap(&proc.as, extra_va, 12288U) == ZZ_OK);
    *(uint16_t *)(void *)(user_page + 0x700U) = 1024U;
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(zzapp_gl, DrawElements)),
             &frame, (uint32_t[9]){ 4U, 1U, 0x1403U, user_va + 0x700U });
    CHECK(draw_elements_calls == 0U);
    CHECK(frame.r[0] == (uint32_t)ZZ_EFAULT);

    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(zzapp_gl, TexImage2D)),
             &frame, (uint32_t[9]){ 0x0de1U, 0U, 4U, 32U, 32U, 0U,
                                    0x1908U, 0x1401U, extra_va });
    CHECK(texture_upload_calls == 0U);
    CHECK(frame.r[0] == (uint32_t)ZZ_EFAULT);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(zzapp_gl, ReadPixels)),
             &frame, (uint32_t[9]){ 0U, 0U, 1U, 1U, 0x1908U, 0x1401U,
                                    WINDOW + 0x1000U });
    CHECK(read_pixels_calls == 0U);
    CHECK(frame.r[0] == (uint32_t)ZZ_EFAULT);
    dispatch(ZZAPP_SVC_GL_FIRST + zzapp_svc_gl_count(), &frame,
             (uint32_t[9]){ 0U });
    CHECK(frame.r[0] == (uint32_t)ZZ_ENOSYS);
#if ZZAPP_PERF_COUNTERS
    CHECK(zzapp_svc_gl_telemetry_swap(0U, 999999U, &snapshot) == 0);
    CHECK(snapshot.descriptor_failures == 1U);
    CHECK(snapshot.pointer_failures >= 4U);
    CHECK(snapshot.array_element_checks >= 2U);
#endif
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
    dispatch(api_slot(offsetof(zzapp_api, gl_bind)), &frame, (uint32_t[9]){ 1U });
    CHECK(frame.r[0] != 0U);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(zzapp_gl, TexSubImage2D)),
             &frame, (uint32_t[9]){ 0x0de1U, 0U, 0U, 0U, 1U, 1U,
                                    0x1908U, 0x1401U, user_va + 0x100U });
    CHECK(texture_sub_upload_calls == 1U);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(zzapp_gl, ReadPixels)),
             &frame, (uint32_t[9]){ 0U, 0U, 1U, 1U, 0x1908U, 0x1401U,
                                    user_va + 0x200U });
    CHECK(read_pixels_calls == 1U);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(zzapp_gl, Build2DMipmaps)),
             &frame, (uint32_t[9]){ 0x0de1U, 4U, 1U, 1U, 0x1908U, 0x1401U,
                                    user_va + 0x240U });
    CHECK(build_mipmaps_calls == 1U);


    proc.heap_base = 0U;
    proc.heap_break = 1U << 20;
    proc.heap_frame_count = 1U;
    dispatch(api_slot(offsetof(zzapp_api, heap_shrink)), &frame,
             (uint32_t[9]){ 1U << 20, WINDOW + 0x100U });
    CHECK(frame.r[0] == (uint32_t)ZZ_EFAULT);
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
    dispatch(api_slot(offsetof(zzapp_api, gl_bind)), &frame, (uint32_t[9]){ 1U });
    CHECK(frame.r[0] != 0U);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(zzapp_gl, GetIntegerv)),
             &frame, (uint32_t[9]){ 0x0ba2U, user_va + 0xffcU });
    CHECK(getter_calls == 0U);
    CHECK(frame.r[0] == (uint32_t)ZZ_EFAULT);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(zzapp_gl, PixelStorei)),
             &frame, (uint32_t[9]){ 0x0d04U, 1024U });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(zzapp_gl, ReadPixels)),
             &frame, (uint32_t[9]){ 0U, 0U, 1U, 1U, 0x1908U, 0x1401U,
                                    user_va + 0xffcU });
    CHECK(read_pixels_calls == 0U);
    CHECK(frame.r[0] == (uint32_t)ZZ_EFAULT);
    message = user_page + 0x600U;
    memset(message, 0xa5, 32U);
    dispatch(api_slot(offsetof(zzapp_api, msg_poll)), &frame,
             (uint32_t[9]){ user_va + 0x600U });
    CHECK(frame.r[0] == 1U);
    CHECK(msg_poll_calls == 1U);
    CHECK(message[0] == 0U);
    dispatch(api_slot(offsetof(zzapp_api, heap_range)), &frame,
             (uint32_t[9]){ 0U, 0U });
    CHECK(heap_range_calls == 1U);
    dispatch(api_slot(offsetof(zzapp_api, file_seek)), &frame,
             (uint32_t[9]){ 1U, 0U, 0U, 0U, 0U, 0U });
    CHECK(file_seek_calls == 1U);
    proc.generation = 1U;
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(zzapp_gl, Begin)),
             &frame, (uint32_t[9]){ 4U });
    CHECK(gl_begin_calls == 0U);
    proc.generation = 0U;
    zzapp_svc_gl_release(0U, 0U);
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(zzapp_gl, Begin)),
             &frame, (uint32_t[9]){ 4U });
    CHECK(gl_begin_calls == 0U);
    CHECK(frame.r[0] == (uint32_t)ZZ_ENOSYS);
    CHECK(frame.r[0] == (uint32_t)ZZ_ENOSYS);
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
    svc_test_current_thread = 1;
    dispatch(api_slot(offsetof(zzapp_api, gl_bind)), &frame,
             (uint32_t[9]){ 1U });
    CHECK(frame.r[0] != (uint32_t)ZZ_EFAULT);
    svc_test_current_thread = 0;
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
    dispatch(api_slot(offsetof(zzapp_api, gl_bind)), &frame, (uint32_t[9]){ 1U });
    for (i = 0U; i < sizeof(vector_pnames) / sizeof(vector_pnames[0]); ++i) {
        dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(zzapp_gl, GetIntegerv)),
                 &frame, (uint32_t[9]){ vector_pnames[i], user_va + 0xffcU });
        CHECK(getter_calls == 0U);
        CHECK(frame.r[0] == (uint32_t)ZZ_EFAULT);
    }
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(zzapp_gl, GetIntegerv)),
             &frame, (uint32_t[9]){ vector_pnames[0], user_va + 0x500U });
    CHECK(getter_calls == 1U);
    CHECK(getter_pointer != (uintptr_t)(user_va + 0x500U));
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
    dispatch(api_slot(offsetof(zzapp_api, gl_bind)), &frame, (uint32_t[9]){ 1U });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(zzapp_gl, PixelStorei)),
             &frame, (uint32_t[9]){ 0x0d04U, 1024U });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(zzapp_gl, Begin)),
             &frame, (uint32_t[9]){ 4U });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(zzapp_gl, PixelStorei)),
             &frame, (uint32_t[9]){ 0x0d04U, 0U });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(zzapp_gl, End)),
             &frame, (uint32_t[9]){ 0U });
    dispatch(ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(zzapp_gl, ReadPixels)),
             &frame, (uint32_t[9]){ 0U, 0U, 1U, 1U, 0x1908U, 0x1401U,
                                    user_va + 0xffcU });
    CHECK(gl_begin_calls == 1U);
    CHECK(gl_end_calls == 1U);
    CHECK(read_pixels_calls == 0U);
    CHECK(frame.r[0] == (uint32_t)ZZ_EFAULT);
    teardown_dispatch();
}
static void test_svc_number_comes_from_r12(void)
{
    zzapp_svc_frame_t frame;
    uint32_t time = api_slot(offsetof(zzapp_api, time_us));

    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    *(uint32_t *)(void *)user_page = 0xef000000U | time;
    dispatch(UINT32_MAX, &frame,
             (uint32_t[9]){ 0U });
    CHECK(frame.r[0] == (uint32_t)ZZ_ENOSYS);
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
    svc_test_current_thread = 1;
    dispatch(api_slot(offsetof(zzapp_api, should_stop)), &frame,
             (uint32_t[9]){ 0U });
    CHECK(frame.r[0] == 1U);
    CHECK(should_stop_calls == 1U);
    CHECK(zzapp_svc_last_number[1] ==
          api_slot(offsetof(zzapp_api, should_stop)));
    svc_test_current_thread = 0;
    teardown_dispatch();
}
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
        zzapp_svc_entry_stamp[0] = svc_counter_now;
        dispatch(api_slot(offsetof(zzapp_api, should_stop)), &frame,
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
        gl_slot(offsetof(zzapp_gl, Vertex2f));

    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    dispatch(api_slot(offsetof(zzapp_api, gl_bind)), &frame,
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
    const zzapp_api *table;
    zzapp_svc_frame_t frame;

    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    CHECK(zzapp_svc_set_veneer_alias(0U, 0xd0000000U) == ZZ_OK);
    CHECK(zzapp_svc_set_table_aliases(0U, 0xd0001000U, 0xd0002000U) == ZZ_OK);
    table = zzapp_svc_api_for_slot(0U, &api);
    CHECK(table != NULL);
    CHECK((uintptr_t)table->exit == 0xd0000000U);
    CHECK((uintptr_t)table->build_id == 0xd0001000U +
          offsetof(struct { zzapp_api table; char build_id[128]; }, build_id));
    dispatch(api_slot(offsetof(zzapp_api, gl_bind)), &frame,
             (uint32_t[9]){ 1U });
    CHECK(frame.r[0] == 0xd0002000U);
    teardown_dispatch();
}

#if ZZAPP_PERF_COUNTERS
static void test_gl_telemetry_snapshot_and_rate_limit(void)
{
    zzapp_svc_frame_t frame;
    zzapp_gl_telemetry_snapshot_t snapshot;
    uint32_t begin = ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(zzapp_gl, Begin));
    uint32_t end = ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(zzapp_gl, End));
    uint32_t vertex = ZZAPP_SVC_GL_FIRST + gl_slot(offsetof(zzapp_gl, Vertex2f));
    uint32_t i;

    setup_dispatch();
    if (!user_page) {
        teardown_dispatch();
        return;
    }
    zzapp_svc_gl_telemetry_reset(0U);
    dispatch(api_slot(offsetof(zzapp_api, gl_bind)), &frame, (uint32_t[9]){ 1U });
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
    CHECK(snapshot.top_slots[0] == gl_slot(offsetof(zzapp_gl, Begin)));
    CHECK(snapshot.top_counts[0] == 5U);
    CHECK(snapshot.top_slots[1] == gl_slot(offsetof(zzapp_gl, End)));
    CHECK(snapshot.top_counts[1] == 3U);
    CHECK(snapshot.top_slots[2] == gl_slot(offsetof(zzapp_gl, Vertex2f)));
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

int main(void)
{
    test_table_copy_uses_only_veneers();
    test_gl_table_copy_uses_only_veneers();
    test_pointer_checks();
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
    test_svc_number_comes_from_r12();
    test_fast_no_lock_calls();
    test_svc_float_frame_arguments();
    test_rejected_pixel_store_does_not_change_validation();
#if ZZAPP_PERF_COUNTERS
    test_gl_telemetry_snapshot_and_rate_limit();
#endif
    test_slot_aliases();
    printf("svc_test: %u checks, %d failures\n", checks, failures);
    return failures != 0;
}
