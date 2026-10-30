/* SPDX-License-Identifier: GPL-3.0-or-later
 * Aurora Clock native VideoOut/controller adapter. See native_platform.c for
 * upstream ABI and tiled-layout provenance. Single-threaded, single instance.
 */
#ifndef AURORA_NATIVE_PLATFORM_H
#define AURORA_NATIVE_PLATFORM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The scene writes host uint32_t values 0xAARRGGBB, not RGBA byte order. */
enum {
    PLATFORM_WIDTH = 1920,
    PLATFORM_HEIGHT = 1080,
    PLATFORM_STRIDE = 1920
};

/* Open/present return 0 on success, -1 on error. Open unwinds partial startup. */
int platform_open(void);
/* Available only between successful open and close; never a scanout address. */
uint32_t *platform_pixels(void);
/* Copies the frame and queues a flip. Waits for the previous flip before reuse. */
int platform_present(void);
/* 1: Circle/Options after release, 0: continue, -1: controller/API error. */
int platform_should_close(void);
/* Idempotent cleanup. On failed VideoOut close, retain its backing memory for
 * process teardown rather than free memory the display might still access.
 * Cleanup failures are logged and retained by platform_error(). */
void platform_close(void);
/* First failure since open, including the native API's hexadecimal result. */
const char *platform_error(void);

#ifdef __cplusplus
}
#endif
#endif
