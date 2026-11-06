/*
 * Shared state for the SDL2 host runner. Not an app header.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ZZAPP_HOST_INTERNAL_H
#define ZZAPP_HOST_INTERNAL_H

#include "zzapp.h"

#include <stdint.h>

#define HOST_FD_BASE 3
#define HOST_FD_COUNT 16
#define HOST_MSG_CAP 64u
#define HOST_MSG_BYTES 32u
#define HOST_AUDIO_FRAMES 8192u
#define HOST_AUDIO_RATE 22050u
#define HOST_VISIBLE_LIMIT (16u << 20)
#define HOST_VISIBLE_SLOTS 64u
#define HOST_MAP_LIMIT (16u << 20)
#define HOST_ARENA_BYTES (8u << 20)
#define HOST_INPUT_CAP 256u
#define HOST_PATH_MAX 255u

struct host_options {
    const char *root;
    int scale;
    int headless;
    int abs_paths;
    uint32_t frames_limit;
    const char *input_path;
    int audio_null;
    uint32_t audio_frames;
    const char *audio_dump;
    const char *msg_script;
    const char *msg_out;
    const char *dump_front;
    int selftest;
};

struct host_options *host_options(void);
int host_sdl_video_ok(void);
int host_stop_requested(void);
void host_request_stop(void);
int host_stop_event_pending(void);

uint64_t host_time_us(void);
uint32_t host_time_ms(void);
void host_pump(void);

int host_files_init(const char *root, int abs_paths);
void host_files_shutdown(void);
void host_files_bind(zzapp_api *api);

int host_display_init(void);
void host_display_shutdown(void);
void host_display_bind(zzapp_api *api);
void host_display_pump(void);
void host_display_dump(const char *path);
int host_display_selftest(void);
uint32_t host_display_flips(void);

int host_input_init(void);
void host_input_shutdown(void);
void host_input_bind(zzapp_api *api);
void host_input_pump(void);
int host_input_selftest(void);

int host_audio_init(void);
void host_audio_shutdown(void);
void host_audio_bind(zzapp_api *api);
void host_audio_pump(void);
uint32_t host_audio_frames_consumed(void);

void host_console_bind(zzapp_api *api);

int host_messages_init(void);
void host_messages_shutdown(void);
void host_messages_bind(zzapp_api *api);
void host_messages_pump(void);

void host_services_bind(zzapp_api *api);
int host_services_selftest(void);

void host_memory_init(void);
void host_memory_shutdown(void);
void host_memory_bind(zzapp_api *api);

#endif
