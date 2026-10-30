/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "scene.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "vendor/stb_truetype.h"
#include "font_data.h"

#define AURORA_WIDTH 480
#define AURORA_HEIGHT 270
#define ASCII_COUNT 128
#define TEXT_CAPACITY 64

typedef struct Glyph {
    size_t offset;
    int width;
    int height;
    int left;
    int top;
    float advance;
} Glyph;

typedef struct FontCache {
    unsigned char *coverage;
    Glyph glyphs[ASCII_COUNT];
    float scale;
} FontCache;

typedef struct ScaleSample {
    int first;
    int second;
    unsigned int weight;
} ScaleSample;

typedef struct TextLayout {
    char text[TEXT_CAPACITY];
    float positions[TEXT_CAPACITY];
    float width;
    int top;
    int bottom;
    size_t length;
} TextLayout;

typedef struct RibbonColumn {
    float center[3];
    float inverse_width[3];
    float intensity[3];
} RibbonColumn;

struct ClockScene {
    uint32_t background[AURORA_WIDTH * AURORA_HEIGHT];
    ScaleSample *horizontal;
    ScaleSample *vertical;
    uint32_t *scaled_rows;
    const char *error;
    stbtt_fontinfo font;
    FontCache clock_font;
    FontCache date_font;
    TextLayout clock_text;
    TextLayout date_text;
    RibbonColumn columns[AURORA_WIDTH];
    int width;
    int height;
    int cached_hour;
    int cached_minute;
    int cached_year;
    int cached_yday;
    float scale;
};

static int scene_fail(ClockScene *scene, const char *message)
{
    if (scene) scene->error = message;
    return -1;
}

static void destroy_font(FontCache *font)
{
    free(font->coverage);
    memset(font, 0, sizeof(*font));
}

/* The font's coverage is rasterized once per drawable size, never per frame. */
static int create_font(ClockScene *scene, FontCache *font,
                       const char *characters, float pixel_height)
{
    size_t coverage_size = 0;
    size_t character_count = strlen(characters);
    size_t i;
    FontCache result = {0};

    result.scale = stbtt_ScaleForPixelHeight(&scene->font, pixel_height);
    for (i = 0; i < character_count; ++i) {
        unsigned char code = (unsigned char)characters[i];
        Glyph *glyph = &result.glyphs[code];
        int advance;
        int left_bearing;
        int x0, y0, x1, y1;
        size_t size;
        stbtt_GetCodepointHMetrics(&scene->font, code, &advance, &left_bearing);
        stbtt_GetCodepointBitmapBox(&scene->font, code, result.scale,
                                   result.scale, &x0, &y0, &x1, &y1);
        glyph->advance = (float)advance * result.scale;
        glyph->left = x0;
        glyph->top = y0;
        glyph->width = x1 - x0;
        glyph->height = y1 - y0;
        glyph->offset = coverage_size;
        if (glyph->height != 0 &&
            (size_t)glyph->width > SIZE_MAX / (size_t)glyph->height)
            return scene_fail(scene, "Clock glyph is too large");
        size = (size_t)glyph->width * (size_t)glyph->height;
        if (size > SIZE_MAX - coverage_size)
            return scene_fail(scene, "Clock glyph cache is too large");
        coverage_size += size;
    }
    result.coverage = malloc(coverage_size ? coverage_size : 1);
    if (!result.coverage)
        return scene_fail(scene, "Cannot allocate clock glyph cache");
    for (i = 0; i < character_count; ++i) {
        unsigned char code = (unsigned char)characters[i];
        const Glyph *glyph = &result.glyphs[code];
        if (!glyph->width || !glyph->height) continue;
        stbtt_MakeCodepointBitmap(&scene->font, result.coverage + glyph->offset,
                                 glyph->width, glyph->height, glyph->width,
                                 result.scale, result.scale, code);
    }
    *font = result;
    return 0;
}

static void create_samples(ScaleSample *samples, int destination, int source)
{
    int i;
    for (i = 0; i < destination; ++i) {
        double position = ((double)i + 0.5) * source / destination - 0.5;
        if (position < 0.0) position = 0.0;
        if (position > source - 1) position = source - 1;
        samples[i].first = (int)position;
        samples[i].second = samples[i].first + (samples[i].first < source - 1);
        samples[i].weight = (unsigned int)((position - samples[i].first) * 256.0 + 0.5);
    }
}

static int resize_scene(ClockScene *scene, int width, int height)
{
    FontCache clock_font = {0};
    FontCache date_font = {0};
    ScaleSample *horizontal = NULL;
    ScaleSample *vertical = NULL;
    uint32_t *scaled_rows = NULL;
    float sx = (float)width / 1920.0f;
    float sy = (float)height / 1080.0f;
    float scale = sx < sy ? sx : sy;
    /* A one-pixel minimum also permits very small desktop windows. */
    float clock_height = fmaxf(1.0f, 310.0f * scale);
    float date_height = fmaxf(1.0f, 25.0f * scale);

    if ((size_t)width > SIZE_MAX / sizeof(*horizontal) ||
        (size_t)height > SIZE_MAX / sizeof(*vertical) ||
        (size_t)width > SIZE_MAX / (2 * sizeof(*scaled_rows)))
        return scene_fail(scene, "Clock drawable is too large");
    if (create_font(scene, &clock_font, "0123456789:", clock_height) < 0)
        return -1;
    if (create_font(scene, &date_font,
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789, ", date_height) < 0)
        goto failure;
    horizontal = malloc((size_t)width * sizeof(*horizontal));
    vertical = malloc((size_t)height * sizeof(*vertical));
    scaled_rows = malloc((size_t)width * 2 * sizeof(*scaled_rows));
    if (!horizontal || !vertical || !scaled_rows) {
        scene_fail(scene, "Cannot allocate clock scaling cache");
        goto failure;
    }
    create_samples(horizontal, width, AURORA_WIDTH);
    create_samples(vertical, height, AURORA_HEIGHT);
    destroy_font(&scene->clock_font);
    destroy_font(&scene->date_font);
    free(scene->horizontal);
    free(scene->vertical);
    free(scene->scaled_rows);
    scene->clock_font = clock_font;
    scene->date_font = date_font;
    scene->horizontal = horizontal;
    scene->vertical = vertical;
    scene->scaled_rows = scaled_rows;
    scene->width = width;
    scene->height = height;
    scene->scale = scale;
    scene->cached_hour = -1;
    scene->cached_year = -1;
    return 0;

failure:
    destroy_font(&clock_font);
    destroy_font(&date_font);
    free(horizontal);
    free(vertical);
    free(scaled_rows);
    return -1;
}

static void layout_text(const ClockScene *scene, const FontCache *font,
                        TextLayout *layout, float tracking, int tabular_digits)
{
    float pen = 0.0f;
    float digit_advance = 0.0f;
    size_t i;
    int code;
    layout->top = 0;
    layout->bottom = 0;
    layout->length = strlen(layout->text);
    if (tabular_digits) {
        for (code = '0'; code <= '9'; ++code) {
            if (font->glyphs[code].advance > digit_advance)
                digit_advance = font->glyphs[code].advance;
        }
    }
    for (i = 0; i < layout->length; ++i) {
        unsigned char current = (unsigned char)layout->text[i];
        const Glyph *glyph = &font->glyphs[current];
        float advance = glyph->advance;
        float inset = 0.0f;
        if (tabular_digits && current >= '0' && current <= '9') {
            advance = digit_advance;
            inset = (digit_advance - glyph->advance) * 0.5f;
        }
        layout->positions[i] = pen + inset;
        if (glyph->top < layout->top) layout->top = glyph->top;
        if (glyph->top + glyph->height > layout->bottom)
            layout->bottom = glyph->top + glyph->height;
        pen += advance;
        if (i + 1 < layout->length) {
            pen += tracking;
            if (!tabular_digits) {
                pen += (float)stbtt_GetCodepointKernAdvance(
                    &scene->font, current, (unsigned char)layout->text[i + 1])
                    * font->scale;
            }
        }
    }
    layout->width = pen;
}

static void update_time(ClockScene *scene, const struct tm *local)
{
    static const char *const weekdays[] = {
        "SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY"
    };
    static const char *const months[] = {
        "JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE",
        "JULY", "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER"
    };
    if (local->tm_hour != scene->cached_hour ||
        local->tm_min != scene->cached_minute) {
        snprintf(scene->clock_text.text, sizeof(scene->clock_text.text),
                 "%02d:%02d", local->tm_hour, local->tm_min);
        layout_text(scene, &scene->clock_font, &scene->clock_text,
                    6.0f * scene->scale, 1);
        scene->cached_hour = local->tm_hour;
        scene->cached_minute = local->tm_min;
    }
    if (local->tm_year != scene->cached_year ||
        local->tm_yday != scene->cached_yday) {
        snprintf(scene->date_text.text, sizeof(scene->date_text.text),
                 "%s, %s %d, %d", weekdays[local->tm_wday],
                 months[local->tm_mon], local->tm_mday, local->tm_year + 1900);
        layout_text(scene, &scene->date_font, &scene->date_text,
                    3.3f * scene->scale, 0);
        scene->cached_year = local->tm_year;
        scene->cached_yday = local->tm_yday;
    }
}

static uint32_t color_channel(float value)
{
    if (value > 255.0f) return 255;
    if (value < 0.0f) return 0;
    return (uint32_t)value;
}

static void draw_background(ClockScene *scene, double elapsed_seconds)
{
    int x, y;
    /* Keep phase calculations in double precision, even after days of uptime. */
    double a = elapsed_seconds * 0.034;
    double b = elapsed_seconds * 0.021;
    double c = elapsed_seconds * 0.013;

    for (x = 0; x < AURORA_WIDTH; ++x) {
        double u = (double)x / (double)(AURORA_WIDTH - 1);
        double p = u * 2.0 - 1.0;
        RibbonColumn *column = &scene->columns[x];
        float folds = (float)(0.80 + 0.13 * sin(u * 53.0 + b) +
                             0.07 * sin(u * 117.0 - a));
        column->center[0] = (float)(0.32 + 0.12 * sin(p * 2.8 + a) +
                                   0.047 * sin(p * 6.7 - b) + 0.075 * p);
        column->center[1] = (float)(0.39 + 0.13 * sin(p * 2.4 - b + 2.1) +
                                   0.038 * sin(p * 7.9 + c) - 0.085 * p);
        column->center[2] = (float)(0.27 + 0.075 * sin(p * 3.5 + c + 4.0) +
                                   0.037 * sin(p * 5.7 - a));
        column->inverse_width[0] = (float)(1.0 / (0.018 + 0.009 * (1.0 + sin(p * 4.1 + b))));
        column->inverse_width[1] = (float)(1.0 / (0.025 + 0.010 * (1.0 + sin(p * 3.2 - c))));
        column->inverse_width[2] = (float)(1.0 / (0.035 + 0.009 * (1.0 + sin(p * 4.0 + a))));
        column->intensity[0] = folds * (float)(0.67 + 0.20 * sin(u * 3.8 + c));
        column->intensity[1] = (float)(0.47 + 0.12 * sin(u * 5.4 - a));
        column->intensity[2] = (float)(0.44 + 0.13 * sin(u * 4.2 + b));
    }
    for (y = 0; y < AURORA_HEIGHT; ++y) {
        uint32_t *row = scene->background + (size_t)y * AURORA_WIDTH;
        float v = (float)y / (float)(AURORA_HEIGHT - 1);
        float vertical_shade = 1.0f - 0.38f * v;
        for (x = 0; x < AURORA_WIDTH; ++x) {
            const RibbonColumn *column = &scene->columns[x];
            float light[3];
            float u = (float)x / (float)(AURORA_WIDTH - 1);
            float edge = (u - 0.5f) * 2.0f;
            float vignette = (1.0f - 0.27f * edge * edge) * vertical_shade;
            uint32_t hash = (uint32_t)x * UINT32_C(374761393) +
                            (uint32_t)y * UINT32_C(668265263);
            float dither;
            float red, green, blue;
            int ribbon;
            hash = (hash ^ (hash >> 13)) * UINT32_C(1274126177);
            dither = (float)((hash >> 24) & 255u) / 255.0f - 0.5f;
            for (ribbon = 0; ribbon < 3; ++ribbon) {
                float distance = v - column->center[ribbon];
                float narrow = distance * column->inverse_width[ribbon];
                float broad = distance * (distance < 0.0f ? 7.5f : 13.0f);
                float core = 1.0f / (1.0f + narrow * narrow);
                float haze = 1.0f / (1.0f + broad * broad);
                /* A soft upper curtain and a finer, brighter lower fold. */
                light[ribbon] = column->intensity[ribbon] *
                    (0.67f * core * core + 0.33f * haze * haze);
            }
            red = (4.0f + 10.0f * light[0] + 21.0f * light[1] + 90.0f * light[2]) * vignette;
            green = (10.0f + 151.0f * light[0] + 88.0f * light[1] + 32.0f * light[2]) * vignette;
            blue = (23.0f + 123.0f * light[0] + 135.0f * light[1] + 122.0f * light[2]) * vignette;
            row[x] = UINT32_C(0xff000000) |
                     (color_channel(red + dither) << 16) |
                     (color_channel(green + dither) << 8) |
                     color_channel(blue + dither);
        }
    }
}

/* Interpolate independent packed red/blue and green lanes without floats.
 * Pixel centers and clamped edges match linear texture sampling. */
static uint32_t interpolate(uint32_t first, uint32_t second, unsigned int weight)
{
    unsigned int inverse = 256 - weight;
    uint32_t red_blue = (((first & UINT32_C(0x00ff00ff)) * inverse +
                         (second & UINT32_C(0x00ff00ff)) * weight +
                         UINT32_C(0x00800080)) >> 8) & UINT32_C(0x00ff00ff);
    uint32_t green = (((first & UINT32_C(0x0000ff00)) * inverse +
                      (second & UINT32_C(0x0000ff00)) * weight +
                      UINT32_C(0x00008000)) >> 8) & UINT32_C(0x0000ff00);
    return UINT32_C(0xff000000) | red_blue | green;
}

static void scale_row(const ClockScene *scene, int source_y, uint32_t *row)
{
    const uint32_t *source = scene->background + (size_t)source_y * AURORA_WIDTH;
    int x;
    for (x = 0; x < scene->width; ++x) {
        const ScaleSample *sample = &scene->horizontal[x];
        row[x] = interpolate(source[sample->first], source[sample->second], sample->weight);
    }
}

static void scale_background(ClockScene *scene, uint32_t *pixels, int stride)
{
    uint32_t *first = scene->scaled_rows;
    uint32_t *second = first + scene->width;
    int first_y = -1;
    int second_y = -1;
    int x, y;
    /* Reuse horizontal rows across adjacent output scanlines. */
    for (y = 0; y < scene->height; ++y) {
        const ScaleSample *sample = &scene->vertical[y];
        uint32_t *destination = pixels + (size_t)y * (size_t)stride;
        if (sample->first != first_y) {
            if (sample->first == second_y) {
                uint32_t *temporary = first;
                first = second;
                second = temporary;
                second_y = first_y;
            } else {
                scale_row(scene, sample->first, first);
            }
            first_y = sample->first;
        }
        if (sample->second == first_y) {
            memcpy(destination, first, (size_t)scene->width * sizeof(*destination));
            continue;
        }
        if (sample->second != second_y) {
            scale_row(scene, sample->second, second);
            second_y = sample->second;
        }
        for (x = 0; x < scene->width; ++x)
            destination[x] = interpolate(first[x], second[x], sample->weight);
    }
}

static void draw_text(const ClockScene *scene, const FontCache *font,
                      const TextLayout *text, float center_x, float center_y,
                      uint32_t color, unsigned int opacity,
                      uint32_t *pixels, int stride)
{
    float left = center_x - text->width * 0.5f;
    float baseline = center_y - (float)(text->top + text->bottom) * 0.5f;
    size_t i;
    for (i = 0; i < text->length; ++i) {
        const Glyph *glyph = &font->glyphs[(unsigned char)text->text[i]];
        int destination_x, destination_y;
        int x0, y0, x1, y1, x, y;
        if (!glyph->width || !glyph->height) continue;
        destination_x = (int)roundf(left + text->positions[i] + (float)glyph->left);
        destination_y = (int)roundf(baseline + (float)glyph->top);
        x0 = destination_x < 0 ? -destination_x : 0;
        y0 = destination_y < 0 ? -destination_y : 0;
        x1 = glyph->width;
        y1 = glyph->height;
        if (x1 > scene->width - destination_x) x1 = scene->width - destination_x;
        if (y1 > scene->height - destination_y) y1 = scene->height - destination_y;
        for (y = y0; y < y1; ++y) {
            const unsigned char *coverage = font->coverage + glyph->offset +
                                           (size_t)y * (size_t)glyph->width;
            if (x0 >= x1) continue;
            uint32_t *destination = pixels +
                (size_t)(destination_y + y) * (size_t)stride + (destination_x + x0);
            for (x = x0; x < x1; ++x, ++destination) {
                unsigned int alpha = coverage[x] * opacity / 255;
                unsigned int inverse;
                uint32_t old, red, green, blue;
                if (!alpha) continue;
                inverse = 255 - alpha;
                old = *destination;
                red = (((color >> 16) & 255) * alpha +
                       ((old >> 16) & 255) * inverse) / 255;
                green = (((color >> 8) & 255) * alpha +
                         ((old >> 8) & 255) * inverse) / 255;
                blue = ((color & 255) * alpha + (old & 255) * inverse) / 255;
                *destination = UINT32_C(0xff000000) | (red << 16) | (green << 8) | blue;
            }
        }
    }
}

ClockScene *scene_create(void)
{
    ClockScene *scene = calloc(1, sizeof(*scene));
    int font_offset;
    if (!scene) return NULL;
    scene->cached_hour = -1;
    scene->cached_year = -1;
    font_offset = stbtt_GetFontOffsetForIndex(clock_font_data, 0);
    if (font_offset < 0 || !stbtt_InitFont(&scene->font, clock_font_data, font_offset)) {
        scene_destroy(scene);
        return NULL;
    }
    return scene;
}

int scene_draw(ClockScene *scene, const struct tm *local,
               double elapsed_seconds, uint32_t *pixels,
               int width, int height, int stride_pixels)
{
    float drift_x;
    float drift_y;
    float center_x;
    float center_y;
    if (!scene || !local)
        return scene_fail(scene, "Clock drawing requires a scene and local time");
    scene->error = NULL;
    if (local->tm_hour < 0 || local->tm_hour > 23 ||
        local->tm_min < 0 || local->tm_min > 59 ||
        local->tm_wday < 0 || local->tm_wday > 6 ||
        local->tm_mon < 0 || local->tm_mon > 11)
        return scene_fail(scene, "Clock received invalid local calendar time");
    if (!isfinite(elapsed_seconds) || elapsed_seconds < 0.0)
        return scene_fail(scene, "Clock requires finite, nonnegative elapsed time");
    if (!pixels || width <= 0 || height <= 0 || stride_pixels < width ||
        (size_t)height > SIZE_MAX / sizeof(*pixels) / (size_t)stride_pixels)
        return scene_fail(scene, "Clock requires a valid framebuffer and pixel stride");
    if ((width != scene->width || height != scene->height) &&
        resize_scene(scene, width, height) < 0)
        return -1;
    update_time(scene, local);
    draw_background(scene, elapsed_seconds);
    scale_background(scene, pixels, stride_pixels);
    /* Deliberately tiny, nonrepeating-in-the-short-term placement drift. */
    drift_x = (float)sin(elapsed_seconds * 0.009) * 9.0f * scene->scale;
    drift_y = (float)sin(elapsed_seconds * 0.007) * 6.0f * scene->scale;
    center_x = (float)width * 0.5f + drift_x;
    center_y = (float)height * 0.5f + drift_y;
    draw_text(scene, &scene->clock_font, &scene->clock_text,
              center_x, center_y - 16.0f * scene->scale,
              UINT32_C(0x00f0f5ef), 248, pixels, stride_pixels);
    draw_text(scene, &scene->date_font, &scene->date_text,
              center_x, center_y + 150.0f * scene->scale,
              UINT32_C(0x00b3ccce), 221, pixels, stride_pixels);
    return 0;
}

void scene_destroy(ClockScene *scene)
{
    if (!scene) return;
    destroy_font(&scene->clock_font);
    destroy_font(&scene->date_font);
    free(scene->horizontal);
    free(scene->vertical);
    free(scene->scaled_rows);
    free(scene);
}

const char *scene_error(const ClockScene *scene)
{
    if (!scene) return "Clock scene is unavailable";
    return scene->error ? scene->error : "";
}
