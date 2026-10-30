/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "native_platform.h"
#include "scene.h"
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

extern uint64_t sceKernelGetProcessTime(void);
extern int sceKernelUsleep(unsigned int);
extern int aurora_native_log(const char *, size_t);

static volatile sig_atomic_t stopping;
static void request_stop(int signal_number) { stopping = signal_number; }
static void log_message(const char *message)
{
    aurora_native_log(message, strlen(message));
    aurora_native_log("\n", 1);
}

int main(void)
{
    int result = 1;
    ClockScene *scene = NULL;
    uint64_t frames = 0;
    log_message("Aurora Clock: native VideoOut, no SDL");
    if (signal(SIGTERM, request_stop) == SIG_ERR) {
        log_message("Cannot install termination handler");
        return 1;
    }
    if (platform_open() < 0) {
        log_message(platform_error());
        goto cleanup;
    }
    scene = scene_create();
    if (!scene) {
        log_message("Cannot create clock scene");
        goto cleanup;
    }
    uint64_t start = sceKernelGetProcessTime();
    while (!stopping) {
        uint64_t frame_start = sceKernelGetProcessTime();
        int close = platform_should_close();
        if (close < 0) {
            log_message(platform_error());
            goto cleanup;
        }
        if (close) break;
        time_t now = time(NULL);
        struct tm local;
        if (now == (time_t)-1 || !localtime_r(&now, &local)) {
            log_message("Cannot read local time");
            goto cleanup;
        }
        if (scene_draw(scene, &local, (double)(frame_start - start) / 1000000.0,
                       platform_pixels(), 1920, 1080, 1920) < 0) {
            log_message(scene_error(scene));
            goto cleanup;
        }
        if (platform_present() < 0) {
            log_message(platform_error());
            goto cleanup;
        }
        if (++frames == 1) log_message("First native clock frame presented");
        uint64_t elapsed = sceKernelGetProcessTime() - frame_start;
        if (elapsed < 33333) sceKernelUsleep((unsigned int)(33333 - elapsed));
    }
    result = 0;
cleanup:
    if (scene) scene_destroy(scene);
    platform_close();
    char message[96];
    snprintf(message, sizeof(message), "Aurora Clock exit: status=%d, frames=%llu",
             result, (unsigned long long)frames);
    log_message(message);
    return result;
}
