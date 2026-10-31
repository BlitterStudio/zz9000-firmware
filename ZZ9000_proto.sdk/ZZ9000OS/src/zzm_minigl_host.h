/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ZZM_MINIGL_HOST_H
#define ZZM_MINIGL_HOST_H

#include <stddef.h>
#include <stdint.h>

/* The module owns this table.  Firmware retains it only while the module has
 * a strong reference.  Every table is append-only and starts with version and
 * size so a newer minor can append fields safely. */
#define ZZM_MINIGL_OPS_V1 0x00010000U
#define ZZM_MINIGL_WORKER_OPS_V1 0x00010000U
#define ZZM_GRAPHICS_HW_V1 0x00010000U
#define ZZM_MINIGL_POLL_BUDGET 1U

enum zzm_minigl_image_flavour {
    ZZM_MINIGL_IMAGE_STANDARD = 0U,
    ZZM_MINIGL_IMAGE_RASTERIX_EF = 1U,
};

enum zzm_minigl_reset_result {
    ZZM_MINIGL_RESET_BUSY = 0,
    ZZM_MINIGL_RESET_QUIESCENT = 1,
    ZZM_MINIGL_RESET_FAILED = -1,
};

typedef struct zzm_minigl_worker_ops {
    uint32_t version;
    uint32_t size;
    int (*has_work)(void *context);
    /* Core 1 only; bounded and non-reentrant. */
    int (*poll)(void *context, uint32_t budget);
} zzm_minigl_worker_ops_t;


typedef struct zzm_minigl_wire_open {
    uint32_t width;
    uint32_t height;
    uint32_t flags;
} zzm_minigl_wire_open_t;

typedef struct zzm_minigl_ops_v1 {
    uint32_t version;
    uint32_t size;
    void (*doorbell)(void *context);
    int (*has_work)(void *context);
    int (*poll)(void *context, uint32_t budget);
    int (*open_68k)(void *context, const zzm_minigl_wire_open_t *request,
                    uint32_t *handle);
    void (*close_68k)(void *context, uint32_t handle);
    int (*transport_acquire)(void *context, uint32_t *generation);
    int (*transport_release)(void *context);
    int (*begin_reset)(void *context);
    int (*reset_poll)(void *context);
    void (*abort_reset)(void *context);
    int (*begin_unload)(void *context);
    void (*fini)(void *context);
    int (*app_bind)(void *context, uint32_t surface_id, const void **out_gl);
    int (*app_swap)(void *context);
    void (*app_unbind)(void *context);
    void (*app_reclaim)(void *context, uint32_t generation);
} zzm_minigl_ops_v1_t;

typedef struct zzm_minigl_boot_caps {
    uint32_t version;
    uint32_t size;
    uint32_t image_flavour;
    uint32_t has_rasterix_ef;
    uint32_t rasterix_csr_abi;
    uint32_t afi3_64bit;
    uint32_t has_pl330;
} zzm_minigl_boot_caps_t;

typedef uint32_t (*zzm_graphics_hw_read_fn)(void *context, uint32_t offset);
typedef void (*zzm_graphics_hw_write_fn)(void *context, uint32_t offset,
                                         uint32_t value);
typedef int (*zzm_graphics_hw_dma_fn)(void *context, uintptr_t source,
                                      uint32_t words, uint32_t burst_words);

typedef struct zzm_graphics_hw {
    uint32_t version;
    uint32_t size;
    const zzm_minigl_boot_caps_t *caps;
    void *context;
    zzm_graphics_hw_read_fn csr_read;
    zzm_graphics_hw_write_fn csr_write;
    zzm_graphics_hw_read_fn fifo_read;
    zzm_graphics_hw_write_fn fifo_write;
    zzm_graphics_hw_dma_fn pl330_start;
    void (*pl330_abort)(void *context);
    void (*pl330_release)(void *context);
    void (*cache_clean)(void *context, uintptr_t address, size_t bytes);
    void (*cache_invalidate)(void *context, uintptr_t address, size_t bytes);
    void (*barrier)(void *context);
} zzm_graphics_hw_t;

/* Registration and service tick. Registration pins execution but does not
 * contribute to the module loader's external-use reference count. */
int zzm_minigl_register(const zzm_minigl_ops_v1_t *ops, void *context,
                        const zzm_minigl_worker_ops_t *worker_ops);
void zzm_minigl_unregister(void);
const zzm_minigl_ops_v1_t *zzm_minigl_ops(void **context);
int zzm_minigl_worker_registered(void);
int zzm_minigl_has_work(void);
void zzm_minigl_doorbell(void);
int zzm_minigl_transport_acquire(uint32_t *generation);
int zzm_minigl_transport_release(void);
/* Called only from core 1's worker/service tick. */
void zzm_minigl_service_tick(int in_api);
int zzm_minigl_begin_reset(void);
int zzm_minigl_reset_poll(void);
void zzm_minigl_abort_reset(void);
void zzm_minigl_on_core1_fault(void);

/* Firmware writes this record once during boot.  Standard images never map or
 * access RasterIX CSR/FIFO addresses. */
void zzm_minigl_boot_caps_init(uint32_t image_flavour);
const zzm_minigl_boot_caps_t *zzm_minigl_boot_caps(void);
const zzm_graphics_hw_t *zzm_minigl_graphics_hw(void);

#endif /* ZZM_MINIGL_HOST_H */
