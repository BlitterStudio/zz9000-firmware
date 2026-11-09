/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "zzapp.h"
#include "zzapp_as.h"
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
    CHECK(zzapp_as_grant_user(as, 0x02000000U, 4096U) == 0);
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
    CHECK(zzapp_svc_check_user(&as, 0x02000000U, 4U, 1) == ZZ_OK);
    CHECK(zzapp_svc_check_user(&as, 0x02100000U, 4U, 1) == ZZ_EFAULT);
    CHECK(zzapp_svc_check_user(&as, 0xfffffff0U, 32U, 0) == ZZ_EFAULT);
    zzapp_as_destroy(&as);
}

int main(void)
{
    test_table_copy_uses_only_veneers();
    test_gl_table_copy_uses_only_veneers();
    test_pointer_checks();
    test_descriptors_cover_callable_slots();
    test_descriptor_shapes();
    printf("svc_test: %u checks, %d failures\n", checks, failures);
    return failures != 0;
}
