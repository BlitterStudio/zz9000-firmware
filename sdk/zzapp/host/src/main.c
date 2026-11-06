/*
 * Host runner entry. Parses launcher options, installs a fully populated
 * API 0.2 table, and calls zz_main. argv[0] is the program name, as on the
 * card. zz_exit does not return.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include "host_internal.h"

#include <SDL.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

static struct host_options opt;
static int stop_flag;
static volatile sig_atomic_t sig_stop;
static int finished;
static int sdl_video;
static int exit_code;
static const char build_id[] = "zzapp-host";
static uint64_t perf_origin;
static uint64_t perf_freq;

struct host_options *host_options(void)
{
    return &opt;
}

int host_sdl_video_ok(void)
{
    return sdl_video;
}

int host_stop_requested(void)
{
    if (sig_stop)
        stop_flag = 1;
    return stop_flag;
}

void host_request_stop(void)
{
    stop_flag = 1;
}

int host_stop_event_pending(void)
{
    return sig_stop || stop_flag;
}

uint64_t host_time_us(void)
{
    if (perf_freq) {
        uint64_t now = SDL_GetPerformanceCounter();
        return (now - perf_origin) * 1000000ull / perf_freq;
    }
    return (uint64_t)clock() * 1000000ull / (uint64_t)CLOCKS_PER_SEC;
}

uint32_t host_time_ms(void)
{
    return (uint32_t)(host_time_us() / 1000ull);
}

void host_pump(void)
{
    host_input_pump();
    host_display_pump();
    host_audio_pump();
    host_messages_pump();
}

static void on_signal(int sig)
{
    (void)sig;
    sig_stop = 1;
}

#ifdef _WIN32
static BOOL WINAPI ctrl_handler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        sig_stop = 1;
        return TRUE;
    }
    return FALSE;
}
#endif

static void install_stop_handlers(void)
{
    signal(SIGINT, on_signal);
#ifdef SIGTERM
    signal(SIGTERM, on_signal);
#endif
#ifdef _WIN32
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
#endif
}

static void usage(FILE *fp)
{
    fputs(
        "zzapp-host runs a host build of a ZZ9000 app.\n"
        "\n"
        "  app [runner options] [--] [app arguments]\n"
        "\n"
        "Runner options:\n"
        "  --root DIR       file root for relative app paths (default .)\n"
        "  --abs            allow paths that contain ':' (no root check)\n"
        "  --scale N        window scale, 1..8 (default 1)\n"
        "  --headless       dummy SDL video/audio, no visible window\n"
        "  --frames N       stop after N presented frames\n"
        "  --input FILE     scripted ZZ_EV_* events\n"
        "  --audio-null     consume PCM on yield instead of SDL audio\n"
        "  --audio-frames N stop after N consumed PCM frames\n"
        "  --audio-dump FILE  write consumed PCM (S16LE stereo)\n"
        "  --msg-script FILE  inject host-to-app messages\n"
        "  --msg-out FILE   record app-to-host messages as hex\n"
        "  --dump-front FILE  write the last presented surface bytes\n"
        "  --help           show this text\n"
        "\n"
        "App arguments after the runner options are passed to zz_main.\n"
        "argv[0] is this program's name. A '--' ends runner options.\n",
        fp);
}

static int need_value(int i, int argc, char **argv, const char **out)
{
    if (i + 1 >= argc || argv[i + 1][0] == 0) {
        fprintf(stderr, "zzapp-host: %s needs a value\n", argv[i]);
        return -1;
    }
    *out = argv[i + 1];
    return 0;
}

static int parse_u32(const char *text, uint32_t *out, uint32_t min, uint32_t max)
{
    char *end = 0;
    unsigned long v;

    if (!text || !*text)
        return -1;
    v = strtoul(text, &end, 10);
    if (!end || *end || v < min || v > max)
        return -1;
    *out = (uint32_t)v;
    return 0;
}

static int parse_args(int argc, char **argv, int *app_from)
{
    int i;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *val = 0;

        if (strcmp(a, "--") == 0) {
            *app_from = i + 1;
            return 0;
        }
        if (a[0] != '-') {
            *app_from = i;
            return 0;
        }
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(stdout);
            exit(0);
        } else if (strcmp(a, "--headless") == 0) {
            opt.headless = 1;
        } else if (strcmp(a, "--abs") == 0) {
            opt.abs_paths = 1;
        } else if (strcmp(a, "--audio-null") == 0) {
            opt.audio_null = 1;
        } else if (strcmp(a, "--root") == 0) {
            if (need_value(i, argc, argv, &val) != 0)
                return -1;
            opt.root = val;
            i++;
        } else if (strcmp(a, "--scale") == 0) {
            uint32_t n = 0;
            if (need_value(i, argc, argv, &val) != 0 ||
                parse_u32(val, &n, 1u, 8u) != 0) {
                fprintf(stderr, "zzapp-host: --scale needs 1..8\n");
                return -1;
            }
            opt.scale = (int)n;
            i++;
        } else if (strcmp(a, "--frames") == 0) {
            if (need_value(i, argc, argv, &val) != 0 ||
                parse_u32(val, &opt.frames_limit, 1u, 1000000u) != 0) {
                fprintf(stderr, "zzapp-host: --frames needs a positive integer\n");
                return -1;
            }
            i++;
        } else if (strcmp(a, "--input") == 0) {
            if (need_value(i, argc, argv, &opt.input_path) != 0)
                return -1;
            i++;
        } else if (strcmp(a, "--audio-frames") == 0) {
            if (need_value(i, argc, argv, &val) != 0 ||
                parse_u32(val, &opt.audio_frames, 1u, 100000000u) != 0) {
                fprintf(stderr, "zzapp-host: --audio-frames needs a positive integer\n");
                return -1;
            }
            i++;
        } else if (strcmp(a, "--audio-dump") == 0) {
            if (need_value(i, argc, argv, &opt.audio_dump) != 0)
                return -1;
            i++;
        } else if (strcmp(a, "--msg-script") == 0) {
            if (need_value(i, argc, argv, &opt.msg_script) != 0)
                return -1;
            i++;
        } else if (strcmp(a, "--msg-out") == 0) {
            if (need_value(i, argc, argv, &opt.msg_out) != 0)
                return -1;
            i++;
        } else if (strcmp(a, "--dump-front") == 0) {
            if (need_value(i, argc, argv, &opt.dump_front) != 0)
                return -1;
            i++;
        } else {
            fprintf(stderr, "zzapp-host: unknown option %s\n", a);
            return -1;
        }
    }
    *app_from = argc;
    return 0;
}

static void api_exit(int code)
{
    exit_code = code;
    host_request_stop();
    /* host_finish is also an atexit hook. exit() runs it once. */
    exit(code);
}

static void api_yield(void)
{
    host_pump();
}

static int api_should_stop(void)
{
    return host_stop_requested() ? 1 : 0;
}

static uint64_t api_time_us(void)
{
    return host_time_us();
}

static void api_sleep_us(uint32_t us)
{
    uint64_t deadline = host_time_us() + us;

    while (!host_stop_requested() && host_time_us() < deadline) {
        uint64_t left = deadline - host_time_us();
        if (left > 2000ull)
            SDL_Delay((Uint32)(left / 1000ull));
        else
            host_pump();
    }
}

static void api_log(const char *msg, uint32_t len)
{
    if (!msg)
        return;
    if (len > 1024u)
        len = 1024u;
    if (len)
        fwrite(msg, 1, len, stdout);
    if (len == 0u || msg[len - 1u] != '\n')
        fputc('\n', stdout);
    fflush(stdout);
}

static void bind_lifecycle(zzapp_api *api)
{
    api->exit = api_exit;
    api->yield = api_yield;
    api->should_stop = api_should_stop;
    api->time_us = api_time_us;
    api->sleep_us = api_sleep_us;
    api->log = api_log;
}

static void host_finish(void)
{
    if (finished)
        return;
    finished = 1;
    host_pump();
    if (opt.dump_front)
        host_display_dump(opt.dump_front);
    fprintf(stderr, "zzapp-host: exit=%d flips=%u audio_frames=%u\n", exit_code,
            host_display_flips(), host_audio_frames_consumed());
    host_audio_shutdown();
    host_display_shutdown();
    host_input_shutdown();
    host_messages_shutdown();
    host_files_shutdown();
    host_memory_shutdown();
    if (sdl_video || perf_freq)
        SDL_Quit();
}

static int run_selftest(void)
{
    int rc;

    rc = host_services_selftest();
    if (rc) {
        fprintf(stderr, "zzapp-host: service selftest %d\n", rc);
        return 1;
    }
    rc = host_display_selftest();
    if (rc) {
        fprintf(stderr, "zzapp-host: display selftest %d\n", rc);
        return 1;
    }
    rc = host_input_selftest();
    if (rc) {
        fprintf(stderr, "zzapp-host: input selftest %d\n", rc);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    static zzapp_api api;
    int app_from = argc;
    int app_argc;
    char **app_argv;
    int i;
    const char *self;

    opt.root = ".";
    opt.scale = 1;
    self = getenv("ZZAPP_HOST_SELFTEST");
    if (self && self[0] && strcmp(self, "0") != 0)
        opt.selftest = 1;
    if (parse_args(argc, argv, &app_from) != 0)
        return 10;
    install_stop_handlers();
    if (opt.headless) {
        SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
        SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    }
    if (SDL_Init(SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "zzapp-host: SDL timer init failed: %s\n", SDL_GetError());
        return 10;
    }
    perf_freq = SDL_GetPerformanceFrequency();
    perf_origin = SDL_GetPerformanceCounter();
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) == 0)
        sdl_video = 1;
    else
        fprintf(stderr, "zzapp-host: SDL video init failed: %s\n", SDL_GetError());
    atexit(host_finish);
    host_memory_init();
    if (host_files_init(opt.root, opt.abs_paths) != 0)
        return 10;
    if (host_display_init() != 0 || host_input_init() != 0 ||
        host_audio_init() != 0 || host_messages_init() != 0)
        return 10;
    memset(&api, 0, sizeof api);
    api.size = (uint32_t)sizeof api;
    api.version = ZZAPP_API_VERSION;
    api.build_id = build_id;
    bind_lifecycle(&api);
    host_memory_bind(&api);
    host_files_bind(&api);
    host_display_bind(&api);
    host_input_bind(&api);
    host_audio_bind(&api);
    host_console_bind(&api);
    host_messages_bind(&api);
    host_services_bind(&api);
    zzapp_api_ptr = &api;
    if (opt.selftest) {
        exit_code = run_selftest();
        return exit_code;
    }
    app_argc = argc - app_from + 1;
    app_argv = (char **)calloc((size_t)app_argc + 1u, sizeof *app_argv);
    if (!app_argv)
        return 10;
    app_argv[0] = argv[0];
    for (i = 0; i < argc - app_from; i++)
        app_argv[i + 1] = argv[app_from + i];
    exit_code = zz_main(app_argc, app_argv);
    free(app_argv);
    return exit_code;
}
