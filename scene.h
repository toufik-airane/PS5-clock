/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef AURORA_CLOCK_SCENE_H
#define AURORA_CLOCK_SCENE_H

#include <stdint.h>
#include <time.h>

typedef struct ClockScene ClockScene;

/* The caller owns the opaque 0xAARRGGBB framebuffer; stride is in pixels.
 * Drawing returns 0 on success, -1 on failure (see scene_error).
 * Glyph and scaling caches allocate only on creation or drawable resize. */
ClockScene *scene_create(void);
int scene_draw(ClockScene *scene, const struct tm *local,
               double elapsed_seconds, uint32_t *pixels,
               int width, int height, int stride_pixels);
void scene_destroy(ClockScene *scene);
const char *scene_error(const ClockScene *scene);

#endif
