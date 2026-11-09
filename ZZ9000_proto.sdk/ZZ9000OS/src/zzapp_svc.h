/*
 * App SVC veneer tables and user-pointer validation.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZAPP_SVC_H
#define ZZAPP_SVC_H

#include <stdint.h>

#include "zzapp.h"
#include "zzapp_as.h"
#include "zzapp_kernel.h"


typedef enum {
    ZZAPP_SVC_RETURN_VOID,
    ZZAPP_SVC_RETURN_INT,
    ZZAPP_SVC_RETURN_POINTER,
    ZZAPP_SVC_RETURN_U64
} zzapp_svc_return_t;

typedef enum {
    ZZAPP_SVC_POINTER_IN,
    ZZAPP_SVC_POINTER_OUT,
    ZZAPP_SVC_POINTER_INOUT
} zzapp_svc_pointer_direction_t;

typedef enum {
    ZZAPP_SVC_LENGTH_FIXED,
    ZZAPP_SVC_LENGTH_ARGUMENT,
    ZZAPP_SVC_LENGTH_COUNT,
    ZZAPP_SVC_LENGTH_STRING,
    ZZAPP_SVC_LENGTH_GL_PIXELS,
    ZZAPP_SVC_LENGTH_GL_INDICES
} zzapp_svc_length_t;

typedef struct {
    uint8_t argument;
    uint8_t direction;
    uint8_t length;
    uint8_t length_argument;
    uint16_t element_bytes;
    uint8_t copy;
    uint8_t format_argument;
    uint8_t type_argument;
} zzapp_svc_pointer_t;

typedef struct {
    /* AAPCS core words, including stack words after r0-r3. */
    uint8_t arguments;
    /* Consecutive saved VFP words starting at s0; a double takes two. */
    uint8_t vfp_words;
    uint8_t result;
    zzapp_svc_pointer_t pointers[3];
} zzapp_svc_descriptor_t;

#define ZZAPP_SVC_GL_FIRST 128U
const zzapp_svc_descriptor_t *zzapp_svc_api_descriptor(uint32_t slot);
const zzapp_svc_descriptor_t *zzapp_svc_gl_descriptor(uint32_t slot);

uint32_t zzapp_svc_api_count(void);
uint32_t zzapp_svc_gl_count(void);
uintptr_t zzapp_svc_api_veneer(uint32_t slot);
uintptr_t zzapp_svc_gl_veneer(uint32_t slot);
int zzapp_svc_is_veneer(uintptr_t address);
int zzapp_svc_copy_api(zzapp_api *destination, const zzapp_api *source);
const zzapp_api *zzapp_svc_api_for_slot(uint32_t slot,
                                        const zzapp_api *source);
zzapp_gl *zzapp_svc_gl_for_slot(uint32_t slot);
int zzapp_svc_copy_gl(zzapp_gl *destination, const zzapp_gl *source);

/* A zero-length range is valid; all nonempty ranges must be fully mapped for
 * the requested user access. */
int zzapp_svc_check_user(const zzapp_as_t *as, uint32_t address,
                         uint32_t bytes, int write);

/* Called by the SVC vector after it has saved the user exception frame. */
void zzapp_svc_dispatch(zzapp_arm_frame_t *frame);

#endif
