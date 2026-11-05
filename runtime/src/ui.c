/* The overlay's 2D drawing kit (ui.h). Shapes are drawn by coverage: each image pixel's centre measured against
 * the shape (a rounded box's signed distance), so edges are smooth at any scale. Text through stb_truetype (its
 * implementation is in hud.c), icons through nanosvg. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_truetype.h"
#include "ui.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "../../third_party/nanosvg/nanosvg.h"
#include "../../third_party/nanosvg/nanosvgrast.h"

/* The fonts, built into the program (third_party/fonts/README.md) */
static const unsigned char font_r500[] = {
#embed "../../third_party/fonts/MPLUSRounded1c-Medium.ttf"
};
static const unsigned char font_r700[] = {
#embed "../../third_party/fonts/MPLUSRounded1c-Bold.ttf"
};
static const unsigned char font_r800[] = {
#embed "../../third_party/fonts/MPLUSRounded1c-ExtraBold.ttf"
};
static const unsigned char font_g400[] = {
#embed "../../third_party/fonts/ZenKakuGothicNew-Regular.ttf"
};
static const unsigned char font_g500[] = {
#embed "../../third_party/fonts/ZenKakuGothicNew-Medium.ttf"
};
static const unsigned char font_g700[] = {
#embed "../../third_party/fonts/ZenKakuGothicNew-Bold.ttf"
};
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static stbtt_fontinfo fonts[UI_FONTS];
static int fonts_state;  /* 0 not loaded, 1 loaded, -1 failed */

static int fonts_ready(void) {
    if (fonts_state) return fonts_state > 0;
    const unsigned char *data[UI_FONTS] = {font_r500, font_r700, font_r800, font_g400, font_g500, font_g700};
    fonts_state = 1;
    for (int i = 0; i < UI_FONTS; i++)
        if (!stbtt_InitFont(&fonts[i], data[i], stbtt_GetFontOffsetForIndex(data[i], 0))) {
            fprintf(stderr, "yw4: ui: built-in font %d unreadable\n", i);
            fonts_state = -1;
        }
    return fonts_state > 0;
}

/* ---- pixels */

static void blend(const UiCanvas *c, int x, int y, uint32_t rgb, float a) {
    if (x < 0 || y < 0 || x >= c->w || y >= c->h || a <= 0.0f) return;
    if (a > 1.0f) a = 1.0f;
    uint8_t *p = c->px + ((size_t)y * (size_t)c->w + (size_t)x) * 4;
    const int src[3] = {(int)(rgb >> 16 & 255), (int)(rgb >> 8 & 255), (int)(rgb & 255)};
    /* "over", kept premultiplied: on an opaque picture the same as before (alpha stays 255); on a see-through one
     * (the HUD) the alpha builds up as layers are drawn */
    for (int k = 0; k < 3; k++) p[k] = (uint8_t)((float)p[k] + ((float)src[k] - (float)p[k]) * a + 0.5f);
    p[3] = (uint8_t)(a * 255.0f + (float)p[3] * (1.0f - a) + 0.5f);
}

/* the signed distance from (px, py) to a rounded box (image pixels), negative inside */
static float box_dist(float px, float py, float x0, float y0, float x1, float y1, float r) {
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    float hx = (x1 - x0) * 0.5f - r, hy = (y1 - y0) * 0.5f - r;
    float qx = fabsf(px - cx) - hx, qy = fabsf(py - cy) - hy;
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float in = qx > qy ? qx : qy;
    return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - r;
}

static float cover(float d) { return d <= -0.5f ? 1.0f : d >= 0.5f ? 0.0f : 0.5f - d; }

static void box_a(const UiCanvas *c, float x, float y, float w, float h, float r, uint32_t fill, float fa, float bw,
                  uint32_t border, float ba);

void ui_rect(const UiCanvas *c, float x, float y, float w, float h, uint32_t rgb, float a) {
    box_a(c, x, y, w, h, 0, rgb, a, 0, 0, 1);
}

static void box_a(const UiCanvas *c, float x, float y, float w, float h, float r, uint32_t fill, float fa, float bw,
                  uint32_t border, float ba) {
    float s = c->s;
    float x0 = c->ox + x * s, y0 = c->oy + y * s, x1 = x0 + w * s, y1 = y0 + h * s, R = r * s, B = bw * s;
    float mr = (x1 - x0 < y1 - y0 ? x1 - x0 : y1 - y0) * 0.5f;
    if (R > mr) R = mr;
    float iR = R - B > 0 ? R - B : 0;
    int ix0 = (int)floorf(x0), iy0 = (int)floorf(y0), ix1 = (int)ceilf(x1), iy1 = (int)ceilf(y1);
    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 > c->w) ix1 = c->w;
    if (iy1 > c->h) iy1 = c->h;
    for (int j = iy0; j < iy1; j++)
        for (int i = ix0; i < ix1; i++) {
            float px = (float)i + 0.5f, py = (float)j + 0.5f;
            float co = cover(box_dist(px, py, x0, y0, x1, y1, R));
            if (co <= 0) continue;
            if (B <= 0) {
                blend(c, i, j, fill, co * fa);
                continue;
            }
            float ci = x1 - x0 > 2 * B && y1 - y0 > 2 * B ? cover(box_dist(px, py, x0 + B, y0 + B, x1 - B, y1 - B, iR)) : 0;
            /* the border where the inner box isn't, the fill inside it */
            if (ci < 1.0f) blend(c, i, j, border, co * (1.0f - ci) * ba);
            if (ci > 0.0f) blend(c, i, j, fill, co * ci * fa);
        }
}

void ui_box(const UiCanvas *c, float x, float y, float w, float h, float r, uint32_t fill, float bw, uint32_t border) {
    box_a(c, x, y, w, h, r, fill, 1.0f, bw, border, 1.0f);
}

void ui_card(const UiCanvas *c, float x, float y, float w, float h, float r, uint32_t fill, float fa, float bw,
             uint32_t border, float ba) {
    box_a(c, x, y, w, h, r, fill, fa, bw, border, ba);
}

void ui_box_alpha(const UiCanvas *c, float x, float y, float w, float h, float r, uint32_t rgb, float a) {
    box_a(c, x, y, w, h, r, rgb, a, 0, 0, 1);
}

void ui_shadow(const UiCanvas *c, float x, float y, float w, float h, float r, float blur, uint32_t rgb, float a) {
    enum { STEPS = 12 };  /* layers from the widest to the box itself: a soft edge like CSS box-shadow */
    for (int i = STEPS; i >= 1; i--) {
        float e = blur * 0.5f * (float)i / STEPS;
        box_a(c, x - e, y - e, w + 2 * e, h + 2 * e, r + e, rgb, a / STEPS * 1.6f, 0, 0, 1);
    }
}

/* ---- text */

static int utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    int cp = *p++;
    if (cp >= 0xF0 && p[0] && p[1] && p[2]) cp = (cp & 7) << 18 | (p[0] & 63) << 12 | (p[1] & 63) << 6 | (p[2] & 63), p += 3;
    else if (cp >= 0xE0 && p[0] && p[1]) cp = (cp & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63), p += 2;
    else if (cp >= 0xC0 && p[0]) cp = (cp & 31) << 6 | (p[0] & 63), p += 1;
    *s = (const char *)p;
    return cp;
}

/* draws (c not NULL) or measures str from x on baseline base, image pixels; track: extra space after each letter */
static float text_run(const UiCanvas *c, int f, float px, float x, float base, const char *str, uint32_t rgb,
                      float track) {
    if (!fonts_ready() || f < 0 || f >= UI_FONTS) return 0;
    const stbtt_fontinfo *fi = &fonts[f];
    float sc = stbtt_ScaleForMappingEmToPixels(fi, px);
    float pen = x;
    int prev = 0;
    static unsigned char bm[512 * 512];
    for (const char *p = str; *p;) {
        int cp = utf8_next(&p);
        if (prev) pen += (float)stbtt_GetCodepointKernAdvance(fi, prev, cp) * sc;
        int adv, lsb;
        stbtt_GetCodepointHMetrics(fi, cp, &adv, &lsb);
        if (c && cp > 32) {
            float sx = pen - floorf(pen);
            int x0, y0, x1, y1;
            stbtt_GetCodepointBitmapBoxSubpixel(fi, cp, sc, sc, sx, 0, &x0, &y0, &x1, &y1);
            int gw = x1 - x0, gh = y1 - y0;
            if (gw > 0 && gh > 0 && gw <= 512 && gh <= 512) {
                stbtt_MakeCodepointBitmapSubpixel(fi, bm, gw, gh, gw, sc, sc, sx, 0, cp);
                int gx = (int)floorf(pen) + x0, gy = (int)floorf(base + 0.5f) + y0;
                for (int j = 0; j < gh; j++)
                    for (int i = 0; i < gw; i++)
                        if (bm[j * gw + i]) blend(c, gx + i, gy + j, rgb, (float)bm[j * gw + i] / 255.0f);
            }
        }
        pen += (float)adv * sc + track;
        prev = cp;
    }
    return pen - x;
}

float ui_text_w(int f, float px, const char *str) { return text_run(NULL, f, px, 0, 0, str, 0, 0); }

float ui_baseline(int f, float px, float lh) {
    if (!fonts_ready()) return 0;
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&fonts[f], &asc, &desc, &gap);
    float sc = stbtt_ScaleForMappingEmToPixels(&fonts[f], px);
    return (lh - (float)(asc - desc) * sc) * 0.5f + (float)asc * sc;
}

float ui_text(const UiCanvas *c, int f, float px, float x, float y, float lh, const char *str, uint32_t rgb) {
    if (!fonts_ready()) return 0;
    const stbtt_fontinfo *fi = &fonts[f];
    int asc, desc, gap;
    stbtt_GetFontVMetrics(fi, &asc, &desc, &gap);
    float sc = stbtt_ScaleForMappingEmToPixels(fi, px);
    float content = (float)(asc - desc) * sc;  /* in design units */
    if (lh <= 0) lh = content + (float)gap * sc;  /* "normal" */
    float base = y + (lh - content) * 0.5f + (float)asc * sc;
    float S = c->s;
    float w = text_run(c, f, px * S, c->ox + x * S, c->oy + base * S, str, rgb, 0);
    return w / S;
}

void ui_text_in(const UiCanvas *c, int f, float px, float x, float y, float w, float h, int align, const char *str,
                uint32_t rgb) {
    float tw = align ? ui_text_w(f, px, str) : 0;
    float tx = align == 1 ? x + (w - tw) * 0.5f : align == 2 ? x + w - tw : x;
    ui_text(c, f, px, tx, y, h, str, rgb);
}

/* text broken into lines at spaces to fit w; track_em: letter spacing in ems (CSS letter-spacing) */
float ui_text_wrap(const UiCanvas *c, int f, float px, float track_em, float x, float y, float w, float lh, int align,
                   const char *str, uint32_t rgb) {
    if (!fonts_ready()) return 0;
    const stbtt_fontinfo *fi = &fonts[f];
    int asc, desc, gap;
    stbtt_GetFontVMetrics(fi, &asc, &desc, &gap);
    float sc = stbtt_ScaleForMappingEmToPixels(fi, px), track = track_em * px;
    float content = (float)(asc - desc) * sc;
    if (lh <= 0) lh = content + (float)gap * sc;
    char line[512];
    float ty = y;
    const char *p = str;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        /* the longest run of words that fits */
        size_t best = 0, n = 0;
        for (;;) {
            size_t e = n;
            while (p[e] && p[e] != ' ') e++;
            if (e >= sizeof line) break;
            memcpy(line, p, e), line[e] = 0;
            if (best && text_run(NULL, f, px, 0, 0, line, 0, track) > w) break;
            best = e;
            if (!p[e]) break;
            n = e + 1;
        }
        if (!best) best = strlen(p) < sizeof line - 1 ? strlen(p) : sizeof line - 1;
        memcpy(line, p, best), line[best] = 0;
        if (c) {
            float lw = text_run(NULL, f, px, 0, 0, line, 0, track) - track;
            float lx = align == 1 ? x + (w - lw) * 0.5f : align == 2 ? x + w - lw : x;
            float base = ty + (lh - content) * 0.5f + (float)asc * sc;
            text_run(c, f, px * c->s, c->ox + lx * c->s, c->oy + base * c->s, line, rgb, track * c->s);
        }
        ty += lh, p += best;
    }
    return ty - y;
}

/* ---- images */

void ui_image(const UiCanvas *c, const uint8_t *rgba, int iw, int ih, float x, float y, float w, float h, float r) {
    float s = c->s;
    float x0 = c->ox + x * s, y0 = c->oy + y * s, x1 = x0 + w * s, y1 = y0 + h * s, R = r * s;
    int ix0 = (int)floorf(x0), iy0 = (int)floorf(y0), ix1 = (int)ceilf(x1), iy1 = (int)ceilf(y1);
    for (int j = iy0 < 0 ? 0 : iy0; j < iy1 && j < c->h; j++)
        for (int i = ix0 < 0 ? 0 : ix0; i < ix1 && i < c->w; i++) {
            float px = (float)i + 0.5f, py = (float)j + 0.5f;
            float co = cover(box_dist(px, py, x0, y0, x1, y1, R));
            if (co <= 0) continue;
            /* bilinear */
            float u = (px - x0) / (x1 - x0) * (float)iw - 0.5f, v = (py - y0) / (y1 - y0) * (float)ih - 0.5f;
            int u0 = u < 0 ? 0 : (int)u, v0 = v < 0 ? 0 : (int)v;
            int u1 = u0 + 1 < iw ? u0 + 1 : u0, v1 = v0 + 1 < ih ? v0 + 1 : v0;
            float fu = u < 0 ? 0 : u - (float)u0, fv = v < 0 ? 0 : v - (float)v0;
            const uint8_t *a = rgba + ((size_t)v0 * iw + u0) * 4, *b = rgba + ((size_t)v0 * iw + u1) * 4;
            const uint8_t *cc = rgba + ((size_t)v1 * iw + u0) * 4, *d = rgba + ((size_t)v1 * iw + u1) * 4;
            float col[4];
            for (int k = 0; k < 4; k++)
                col[k] = (a[k] * (1 - fu) + b[k] * fu) * (1 - fv) + (cc[k] * (1 - fu) + d[k] * fu) * fv;
            uint32_t rgb = (uint32_t)(col[0] + 0.5f) << 16 | (uint32_t)(col[1] + 0.5f) << 8 | (uint32_t)(col[2] + 0.5f);
            blend(c, i, j, rgb, co * col[3] / 255.0f);
        }
}

/* ---- icons */

void ui_icon(const UiCanvas *c, const char *elements, float x, float y, float size, float stroke, uint32_t rgb) {
    char svg[2048];
    snprintf(svg, sizeof svg,
             "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"24\" height=\"24\" viewBox=\"0 0 24 24\">"
             "<g fill=\"none\" stroke=\"#000000\" stroke-width=\"%g\" stroke-linecap=\"round\" stroke-linejoin=\"round\">"
             "%s</g></svg>",
             (double)stroke, elements);
    NSVGimage *im = nsvgParse(svg, "px", 96.0f);
    if (!im) return;
    static NSVGrasterizer *rast;
    if (!rast) rast = nsvgCreateRasterizer();
    int n = (int)ceilf(size * c->s) + 2;
    unsigned char *buf = rast && n > 0 && n < 1024 ? calloc((size_t)n * (size_t)n, 4) : NULL;
    if (buf) {
        float fx = c->ox + x * c->s, fy = c->oy + y * c->s;
        int ix = (int)floorf(fx), iy = (int)floorf(fy);
        nsvgRasterize(rast, im, fx - (float)ix, fy - (float)iy, size * c->s / 24.0f, buf, n, n, n * 4);
        for (int j = 0; j < n; j++)
            for (int i = 0; i < n; i++) {
                int a = buf[((size_t)j * (size_t)n + (size_t)i) * 4 + 3];
                if (a) blend(c, ix + i, iy + j, rgb, (float)a / 255.0f);
            }
        free(buf);
    }
    nsvgDelete(im);
}
