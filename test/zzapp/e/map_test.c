/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "host_loader.h"
#include "mock_api.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    static zzapp_mock_app app;
    aar_api api;
    char line[16];
    int code;

    zzapp_mock_api_init(&api, &app);
    if (zzapp_host_map("build/e/map_app.so", &api, 0, 0, &code) != AAR_OK ||
        code != 3 || zzapp_mock_app_log_read(&app, line, sizeof line) != 6 ||
        strcmp(line, "mapped") != 0) {
        fprintf(stderr, "host mapped app failed\n");
        return 1;
    }
    return 0;
}
