/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Native display/tile layout and controller ABI adapted from
 * https://github.com/MiladRTX/ps5-homebrew-starter
 * app/src/screen.cpp, screen.hpp and pad.cpp (GPL-3.0-or-later).
 * Renderer origin: ps5-native-app-boilerplate,
 * Copyright (C) 2026 BlackBearReloaded; starter changes by MiladRTX.
 * No upstream drawing, fonts, or application loop are included here.
 *
 * Flip-status and cleanup ABI declarations cross-referenced against the public
 * SharpProspero Interop/{VideoOut,Kernel,Pad,UserService} bindings,
 * Copyright (C) 2026 SvenGDK, GPL-3.0-or-later. In particular, flipArg is at
 * byte 24 in the 128-byte status, not a vblank counter. These are public
 * homebrew ABIs, not proprietary SDK headers.
 */
#include "native_platform.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern int aurora_native_log(const char *message, size_t length);

extern size_t sceKernelGetDirectMemorySize(void);
extern int sceKernelAllocateDirectMemory(int64_t start, int64_t end,
    size_t length, size_t alignment, int type, int64_t *physical);
extern int sceKernelMapDirectMemory(void **address, size_t length,
    int protection, int flags, int64_t physical, size_t alignment);
extern int sceKernelMunmap(void *address, size_t length);
extern int sceKernelReleaseDirectMemory(int64_t physical, size_t length);

/* SetBufferAttribute2 fills this opaque 80-byte object. Give it native
 * 64-bit alignment even though the starter declares its storage as bytes. */
typedef struct { uint64_t reserved[10]; } VideoAttribute;
typedef struct {
    void *data;
    void *metadata;
    void *reserved0;
    void *reserved1;
} VideoBuffer;
typedef struct {
    uint64_t count;
    uint64_t process_time;
    uint64_t reserved0;
    int64_t flip_arg;
    uint64_t reserved1;
    uint64_t process_time_counter;
    int32_t graphics_queue_count;
    int32_t pending_count;
    int32_t current_buffer;
    uint32_t reserved2;
    uint64_t reserved3[8];
} VideoFlipStatus;

_Static_assert(sizeof(VideoAttribute) == 80, "VideoOut attribute ABI");
_Static_assert(sizeof(VideoBuffer) == 32, "VideoOut buffer ABI");
_Static_assert(sizeof(VideoFlipStatus) == 128, "VideoOut flip status ABI");
_Static_assert(offsetof(VideoFlipStatus, flip_arg) == 24, "VideoOut flip argument ABI");
_Static_assert(offsetof(VideoFlipStatus, current_buffer) == 56, "VideoOut buffer index ABI");

extern int sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void *param);
extern int sceVideoOutClose(int32_t handle);
extern int sceVideoOutSetFlipRate(int32_t handle, int32_t rate);
extern void sceVideoOutSetBufferAttribute2(VideoAttribute *attribute,
    uint64_t format, uint32_t tiling, uint32_t width, uint32_t height,
    uint64_t option, uint32_t dcc_control, uint64_t dcc_clear_color);
extern int sceVideoOutRegisterBuffers2(int32_t handle, int32_t set_index,
    int32_t first_index, VideoBuffer *buffers, int32_t count,
    VideoAttribute *attribute, int32_t category, void *option);
extern int sceVideoOutSubmitFlip(int32_t handle, int32_t index,
    uint32_t mode, int64_t argument);
extern int sceVideoOutGetFlipStatus(int32_t handle, VideoFlipStatus *status);
extern int sceVideoOutWaitVblank(int32_t handle);
extern int sceSystemServiceHideSplashScreen(void);

extern int sceUserServiceInitialize(const void *params);
extern int sceUserServiceGetInitialUser(int32_t *user);
extern int sceUserServiceTerminate(void);
extern int scePadInit(void);
extern int scePadOpen(int32_t user, int32_t type, int32_t index, const void *param);
extern int scePadGetHandle(int32_t user, int32_t type, int32_t index);
extern int scePadRead(int32_t handle, void *samples, int32_t count);
extern int scePadClose(int32_t handle);

enum {
    TILE_SIZE = 128,
    TILES_X = PLATFORM_WIDTH / TILE_SIZE,
    TILES_Y = (PLATFORM_HEIGHT + TILE_SIZE - 1) / TILE_SIZE,
    FRAME_BYTES = 0x1000000,
    DIRECT_BYTES = FRAME_BYTES * 2,
    DIRECT_ALIGNMENT = 0x200000,
    USED_BYTES = TILES_X * TILES_Y * 0x10000,
    PAD_SAMPLE_BYTES = 120,
    PAD_SAMPLE_COUNT = 32
};
#define EXIT_BUTTONS UINT32_C(0x2008) /* Circle | Options, native pad bits. */
#define PAD_INTERCEPTED UINT32_C(0x80000000)

static uint32_t frame[PLATFORM_HEIGHT * PLATFORM_STRIDE];
static _Alignas(16) unsigned char pad_samples[PAD_SAMPLE_COUNT][PAD_SAMPLE_BYTES];
static VideoBuffer buffers[2];
static void *mapped;
static int64_t physical;
static int allocated;
static int video = -1;
static int pad = -1;
static int own_pad;
static int own_user_service;
static int opened;
static int next_buffer;
static int pending_buffer;
static int64_t submitted_flip;
static int flip_pending;
static int exit_armed;
static int first_pad_batch;
static char error_message[192];

static int fail(const char *operation, int result)
{
    char message[192];
    int length = snprintf(message, sizeof(message), "%s failed (0x%08x)\n",
                          operation, (unsigned)result);
    if (length > 0) {
        size_t used = (size_t)length < sizeof(message) ? (size_t)length : sizeof(message) - 1;
        if (!error_message[0]) {
            memcpy(error_message, message, used);
            error_message[used] = '\0';
        }
        (void)aurora_native_log(message, used);
    }
    return -1;
}

/* Exactly the starter's PS5 128x128 / 64-KiB tiled byte addressing. */
static size_t tiled_offset(unsigned x, unsigned y)
{
    uint32_t offset = ((y << 4) & 0x70U) ^ ((y << 5) & 0xf00U) ^
        ((y << 9) & 0x1000U) ^ ((y << 8) & 0x4000U) ^
        ((x << 2) & 0xcU) ^ ((x << 5) & 0x380U) ^
        ((x << 4) & 0x400U) ^ ((x << 6) & 0x800U) ^ ((x << 9) & 0xa000U);
    unsigned block = (y >> 7) * TILES_X + (x >> 7);
    return ((size_t)block << 16) + offset;
}

static void copy_to_scanout(void *scanout)
{
    unsigned char *bytes = scanout;
    for (unsigned by = 0; by < TILES_Y; ++by)
        for (unsigned bx = 0; bx < TILES_X; ++bx)
            for (unsigned y0 = 0; y0 < TILE_SIZE; y0 += 8) {
                unsigned top = by * TILE_SIZE + y0;
                if (top >= PLATFORM_HEIGHT)
                    break;
                for (unsigned x0 = 0; x0 < TILE_SIZE; x0 += 4) {
                    unsigned left = bx * TILE_SIZE + x0;
                    for (unsigned yy = 0; yy < 8 && top + yy < PLATFORM_HEIGHT; ++yy) {
                        const uint32_t *source = frame + (top + yy) * PLATFORM_STRIDE + left;
                        for (unsigned xx = 0; xx < 4; ++xx) {
                            uint32_t argb = source[xx];
                            /* 0xAARRGGBB -> 0xAABBGGRR: RGBA bytes on x86-64.
                             * Attribute format 0x8000000022000000 is RGBA8 sRGB. */
                            uint32_t rgba = (argb & UINT32_C(0xff00ff00)) |
                                ((argb & UINT32_C(0x00ff0000)) >> 16) |
                                ((argb & UINT32_C(0x000000ff)) << 16);
                            *(uint32_t *)(bytes + tiled_offset(left + xx, top + yy)) = rgba;
                        }
                    }
                }
            }
}

/* Match the starter's cache publication for its type-3 WC garlic mapping. */
static void flush_scanout(void *address)
{
    unsigned char *bytes = address;
    for (size_t at = 0; at < USED_BYTES; at += 64)
        __asm__ volatile("clflush (%0)" : : "r"(bytes + at) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}

static int wait_for_flip(void)
{
    unsigned waited_frames = 0;
    while (flip_pending) {
        VideoFlipStatus status = {0};
        int result = sceVideoOutGetFlipStatus(video, &status);
        if (result < 0)
            return fail("sceVideoOutGetFlipStatus", result);
        /* A vblank alone does not establish that a queued flip retired. With
         * one outstanding flip, its tag and index identify the new front;
         * only then may the previous front be written again. */
        if (status.flip_arg == submitted_flip && status.current_buffer == pending_buffer) {
            flip_pending = 0;
            return 0;
        }
        if (++waited_frames > 120)
            return fail("VideoOut flip did not retire within 120 vblanks", -1);
        result = sceVideoOutWaitVblank(video);
        if (result < 0)
            return fail("sceVideoOutWaitVblank", result);
    }
    return 0;
}

int platform_open(void)
{
    int result;
    int32_t user = -1;
    VideoAttribute attribute = {{0}};
    if (opened)
        return 0;
    /* Failed cleanup keeps ownership state intact; never overwrite it. */
    if (video >= 0 || mapped || allocated || pad >= 0 || own_user_service)
        return fail("platform_open: resources still owned", -1);
    error_message[0] = '\0';
    next_buffer = 0;
    submitted_flip = 0;
    flip_pending = 0;
    exit_armed = 0;
    first_pad_batch = 1;

    video = sceVideoOutOpen(0xff, 0, 0, NULL);
    if (video < 0) {
        fail("sceVideoOutOpen", video);
        goto failed;
    }
    size_t pool_size = sceKernelGetDirectMemorySize();
    /* This is the addressable direct-memory pool, not available system RAM. */
    if (pool_size < DIRECT_BYTES || pool_size > INT64_MAX) {
        fail("sceKernelGetDirectMemorySize: unusable pool size", -1);
        goto failed;
    }
    result = sceKernelAllocateDirectMemory(0, (int64_t)pool_size, DIRECT_BYTES,
                                           DIRECT_ALIGNMENT, 3, &physical);
    if (result < 0) {
        fail("sceKernelAllocateDirectMemory", result);
        goto failed;
    }
    allocated = 1;
    result = sceKernelMapDirectMemory(&mapped, DIRECT_BYTES, 0x33, 0,
                                      physical, DIRECT_ALIGNMENT);
    if (result < 0) {
        mapped = NULL;
        fail("sceKernelMapDirectMemory", result);
        goto failed;
    }
    buffers[0] = (VideoBuffer){mapped, NULL, NULL, NULL};
    buffers[1] = (VideoBuffer){(unsigned char *)mapped + FRAME_BYTES, NULL, NULL, NULL};
    /* The final tile row extends beyond 1080 lines. Initialize the padding
     * once; frame copies thereafter touch only visible pixels. */
    memset(buffers[0].data, 0, USED_BYTES);
    memset(buffers[1].data, 0, USED_BYTES);
    flush_scanout(buffers[0].data);
    flush_scanout(buffers[1].data);
    result = sceVideoOutSetFlipRate(video, 0);
    if (result < 0) {
        fail("sceVideoOutSetFlipRate", result);
        goto failed;
    }
    sceVideoOutSetBufferAttribute2(&attribute, UINT64_C(0x8000000022000000),
                                   0, PLATFORM_WIDTH, PLATFORM_HEIGHT, 0, 0, 0);
    result = sceVideoOutRegisterBuffers2(video, 0, 0, buffers, 2, &attribute, 0, NULL);
    if (result < 0) {
        fail("sceVideoOutRegisterBuffers2", result);
        goto failed;
    }

    /* The starter permits initialization by its caller. Only terminate a
     * service initialized here; GetInitialUser supplies the actionable error. */
    own_user_service = sceUserServiceInitialize(NULL) >= 0;
    result = sceUserServiceGetInitialUser(&user);
    if (result < 0 || user < 0) {
        fail("sceUserServiceGetInitialUser", result < 0 ? result : user);
        goto failed;
    }
    result = scePadInit();
    if (result < 0) {
        fail("scePadInit", result);
        goto failed;
    }
    pad = scePadOpen(user, 0, 0, NULL);
    own_pad = pad >= 0;
    if (pad < 0)
        pad = scePadGetHandle(user, 0, 0);
    if (pad < 0) {
        fail("scePadOpen/scePadGetHandle", pad);
        goto failed;
    }
    opened = 1;
    return 0;

failed:
    platform_close();
    return -1;
}

uint32_t *platform_pixels(void)
{
    return opened ? frame : NULL;
}

int platform_present(void)
{
    if (!opened)
        return fail("platform_present: platform not open", -1);
    if (wait_for_flip() < 0)
        return -1;
    copy_to_scanout(buffers[next_buffer].data);
    flush_scanout(buffers[next_buffer].data);
    int64_t argument = submitted_flip + 1;
    int result = sceVideoOutSubmitFlip(video, next_buffer, 1, argument);
    if (result < 0)
        return fail("sceVideoOutSubmitFlip", result);
    submitted_flip = argument;
    pending_buffer = next_buffer;
    flip_pending = 1;
    next_buffer ^= 1;
    if (wait_for_flip() < 0)
        return -1;
    if (submitted_flip == 1) {
        result = sceSystemServiceHideSplashScreen();
        if (result < 0)
            return fail("sceSystemServiceHideSplashScreen", result);
    }
    return 0;
}

int platform_should_close(void)
{
    if (!opened)
        return fail("platform_should_close: platform not open", -1);
    int count = scePadRead(pad, pad_samples, PAD_SAMPLE_COUNT);
    if (count < 0)
        return fail("scePadRead", count);
    if (count > PAD_SAMPLE_COUNT)
        return fail("scePadRead: invalid sample count", count);
    if (!count)
        return 0;
    /* First read may contain history from launching the app. Only its final
     * state can arm exit; never interpret earlier release/press pairs as exit. */
    int start = first_pad_batch ? count - 1 : 0;
    for (int i = start; i < count; ++i) {
        const unsigned char *sample = pad_samples[i];
        uint32_t buttons = (uint32_t)sample[0] | (uint32_t)sample[1] << 8 |
            (uint32_t)sample[2] << 16 | (uint32_t)sample[3] << 24;
        if (buttons & PAD_INTERCEPTED) {
            exit_armed = 0;
        } else if (!(buttons & EXIT_BUTTONS)) {
            exit_armed = 1;
        } else if (exit_armed && !first_pad_batch) {
            return 1;
        }
    }
    first_pad_batch = 0;
    return 0;
}

void platform_close(void)
{
    int result;
    opened = 0;
    if (pad >= 0) {
        result = own_pad ? scePadClose(pad) : 0;
        if (result < 0)
            fail("scePadClose", result);
        else {
            pad = -1;
            own_pad = 0;
        }
    }
    if (own_user_service && pad < 0) {
        result = sceUserServiceTerminate();
        if (result < 0)
            fail("sceUserServiceTerminate", result);
        else
            own_user_service = 0;
    }
    if (video >= 0) {
        /* Match the PS5 SDL backend's close/release lifecycle: close the
         * entire output before releasing its buffers, rather than mixing
         * legacy UnregisterBuffers with this RegisterBuffers2 path. */
        (void)wait_for_flip();
        result = sceVideoOutClose(video);
        if (result < 0)
            fail("sceVideoOutClose", result);
        else {
            video = -1;
            flip_pending = 0;
        }
    }
    if (video < 0 && mapped) {
        result = sceKernelMunmap(mapped, DIRECT_BYTES);
        if (result < 0)
            fail("sceKernelMunmap", result);
        else {
            mapped = NULL;
            memset(buffers, 0, sizeof(buffers));
        }
    }
    if (video < 0 && !mapped && allocated) {
        result = sceKernelReleaseDirectMemory(physical, DIRECT_BYTES);
        if (result < 0)
            fail("sceKernelReleaseDirectMemory", result);
        else {
            allocated = 0;
            physical = 0;
        }
    }
}

const char *platform_error(void)
{
    return error_message;
}
