/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "host_loader.h"
#include "mock_api.h"
#include "mock_window.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    uint8_t *window = zzapp_mock_window_new();
    zzapp_api api;
    char line[16];
    int code;

    if (!window)
        return 1;
    zzapp_mock_api_init(&api, window);
    if (zzapp_host_map("build/e/map_app.so", &api, 0, 0, &code) != ZZ_OK ||
        code != 3 || zzapp_log_read(window, line, sizeof line) != 6 ||
        strcmp(line, "mapped") != 0) {
        fprintf(stderr, "host mapped app failed\n");
        return 1;
    }
    zzapp_mock_window_free(window);
    return 0;
}
