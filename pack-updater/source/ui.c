#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#pragma GCC diagnostic pop

#include "ui.h"

#define FB_W 1280
#define FB_H 720
#define MAX_FONTS 6
#define CACHE_N 512

typedef struct {
    uint32_t cp;
    int w, h, xoff, yoff, adv;
    uint8_t *px;
} Glyph;

static Framebuffer g_fb;
static uint32_t *g_frame;
static uint32_t g_stride;
static int g_ready;

static PlFontData g_pl[MAX_FONTS];
static stbtt_fontinfo g_font[MAX_FONTS];
static int g_nfont;
static Glyph g_cache[CACHE_N];

static uint32_t utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    if (!p || !p[0])
        return 0;
    if (p[0] < 0x80) {
        *s += 1;
        return p[0];
    }
    if ((p[0] & 0xE0) == 0xC0 && p[1]) {
        *s += 2;
        return ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    }
    if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2]) {
        *s += 3;
        return ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    }
    if ((p[0] & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
        *s += 4;
        return ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
               ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    }
    *s += 1;
    return 0xFFFD;
}

static int add_font(PlSharedFontType type)
{
    if (g_nfont >= MAX_FONTS)
        return -1;
    PlFontData *pd = &g_pl[g_nfont];
    if (R_FAILED(plGetSharedFontByType(pd, type)))
        return -1;
    if (!pd->address || pd->size < 128)
        return -1;
    const unsigned char *data = (const unsigned char *)pd->address;
    int offset = stbtt_GetFontOffsetForIndex(data, 0);
    if (offset < 0)
        offset = 0;
    if (!stbtt_InitFont(&g_font[g_nfont], data, offset))
        return -1;
    g_nfont++;
    return 0;
}

static int font_for(uint32_t cp, int *glyph)
{
    for (int i = 0; i < g_nfont; i++) {
        int g = stbtt_FindGlyphIndex(&g_font[i], (int)cp);
        if (g) {
            *glyph = g;
            return i;
        }
    }
    *glyph = 0;
    return g_nfont ? 0 : -1;
}

static Glyph *glyph_get(uint32_t cp, float size)
{
    uint32_t key = (cp & 0xFFFFFFu) | (((uint32_t)((int)size) & 0xFFu) << 24);
    int slot = (int)(key % CACHE_N);
    for (int n = 0; n < CACHE_N; n++) {
        int i = (slot + n) % CACHE_N;
        if (g_cache[i].cp == key && g_cache[i].px)
            return &g_cache[i];
        if (!g_cache[i].cp)
            break;
    }

    int gi = 0;
    int fi = font_for(cp, &gi);
    if (fi < 0)
        return NULL;

    float scale = stbtt_ScaleForPixelHeight(&g_font[fi], size);
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(&g_font[fi], gi, scale, scale, &x0, &y0, &x1, &y1);
    int w = x1 - x0;
    int h = y1 - y0;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    if (w > 128) w = 128;
    if (h > 128) h = 128;

    uint8_t *px = NULL;
    if (w && h) {
        px = (uint8_t *)malloc((size_t)w * (size_t)h);
        if (!px)
            return NULL;
        stbtt_MakeGlyphBitmap(&g_font[fi], px, w, h, w, scale, scale, gi);
    }
    int adv, lsb;
    stbtt_GetGlyphHMetrics(&g_font[fi], gi, &adv, &lsb);

    Glyph *dst = NULL;
    for (int n = 0; n < CACHE_N; n++) {
        int i = (slot + n) % CACHE_N;
        if (!g_cache[i].cp || g_cache[i].cp == key) {
            dst = &g_cache[i];
            break;
        }
    }
    if (!dst) {
        dst = &g_cache[slot];
        free(dst->px);
        memset(dst, 0, sizeof *dst);
    } else if (dst->px) {
        free(dst->px);
        dst->px = NULL;
    }
    dst->cp = key;
    dst->w = w;
    dst->h = h;
    dst->xoff = x0;
    dst->yoff = y0;
    dst->adv = (int)(adv * scale);
    dst->px = px;
    if (dst->adv < 1)
        dst->adv = w + 1;
    return dst;
}

int ui_init(void)
{
    memset(&g_fb, 0, sizeof g_fb);
    g_frame = NULL;
    g_nfont = 0;
    g_ready = 0;
    memset(g_cache, 0, sizeof g_cache);

    if (R_FAILED(plInitialize(PlServiceType_User)))
        return -1;

    add_font(PlSharedFontType_Standard);
    add_font(PlSharedFontType_ChineseSimplified);
    add_font(PlSharedFontType_ChineseTraditional);
    add_font(PlSharedFontType_NintendoExt);
    if (g_nfont <= 0) {
        plExit();
        return -1;
    }

    NWindow *win = nwindowGetDefault();
    if (R_FAILED(framebufferCreate(&g_fb, win, FB_W, FB_H, PIXEL_FORMAT_RGBA_8888, 2))) {
        plExit();
        return -1;
    }
    framebufferMakeLinear(&g_fb);
    g_ready = 1;
    return 0;
}

void ui_exit(void)
{
    if (g_frame) {
        framebufferEnd(&g_fb);
        g_frame = NULL;
    }
    if (g_ready)
        framebufferClose(&g_fb);
    g_ready = 0;
    for (int i = 0; i < CACHE_N; i++) {
        free(g_cache[i].px);
        g_cache[i].px = NULL;
    }
    plExit();
}

void ui_begin(uint32_t bg)
{
    if (!g_ready)
        return;
    uint32_t stride_bytes = 0;
    g_frame = (uint32_t *)framebufferBegin(&g_fb, &stride_bytes);
    g_stride = stride_bytes / 4;
    if (!g_frame)
        return;
    for (int y = 0; y < FB_H; y++) {
        uint32_t *row = g_frame + (size_t)y * g_stride;
        for (int x = 0; x < FB_W; x++)
            row[x] = bg;
    }
}

int ui_text(int x, int y, float size, uint32_t rgba, const char *utf8)
{
    if (!g_frame || !utf8)
        return x;
    int pen_x = x;
    int baseline = y + (int)(size * 0.82f);
    const char *p = utf8;
    while (*p) {
        if (*p == '\n') {
            p++;
            pen_x = x;
            baseline += (int)(size + 8);
            continue;
        }
        uint32_t cp = utf8_next(&p);
        if (!cp)
            break;
        Glyph *g = glyph_get(cp, size);
        if (!g) {
            pen_x += (int)(size * 0.5f);
            continue;
        }
        int gx0 = pen_x + g->xoff;
        int gy0 = baseline + g->yoff;
        uint8_t sr = (uint8_t)(rgba);
        uint8_t sg = (uint8_t)(rgba >> 8);
        uint8_t sb = (uint8_t)(rgba >> 16);
        for (int gy = 0; gy < g->h; gy++) {
            int py = gy0 + gy;
            if (py < 0 || py >= FB_H)
                continue;
            uint32_t *row = g_frame + (size_t)py * g_stride;
            const uint8_t *src = g->px ? g->px + gy * g->w : NULL;
            if (!src)
                continue;
            for (int gx = 0; gx < g->w; gx++) {
                int px = gx0 + gx;
                if (px < 0 || px >= FB_W)
                    continue;
                uint8_t a = src[gx];
                if (!a)
                    continue;
                uint32_t dst = row[px];
                uint8_t dr = (uint8_t)dst;
                uint8_t dg = (uint8_t)(dst >> 8);
                uint8_t db = (uint8_t)(dst >> 16);
                uint8_t da = (uint8_t)(dst >> 24);
                uint8_t nr = (uint8_t)((sr * a + dr * (255 - a)) / 255);
                uint8_t ng = (uint8_t)((sg * a + dg * (255 - a)) / 255);
                uint8_t nb = (uint8_t)((sb * a + db * (255 - a)) / 255);
                uint8_t na = da > a ? da : a;
                row[px] = (uint32_t)nr | ((uint32_t)ng << 8) | ((uint32_t)nb << 16) | ((uint32_t)na << 24);
            }
        }
        pen_x += g->adv;
    }
    return pen_x;
}

void ui_end(void)
{
    if (!g_ready || !g_frame)
        return;
    framebufferEnd(&g_fb);
    g_frame = NULL;
}

void ui_idle(void)
{
    svcSleepThread(16 * 1000 * 1000ULL);
}
