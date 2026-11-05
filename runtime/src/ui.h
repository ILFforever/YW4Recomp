/* A small 2D drawing kit for the in-game overlay (gamemenu.h), on the CPU into an RGBA image: rectangles and
 * rounded boxes with smooth edges and borders, text in the overlay design's fonts (built into the program, see
 * third_party/fonts), and line icons from SVG path data (nanosvg). Everything is placed in the design's own units
 * (CSS pixels of its 1600 x 900 artboard) and drawn at `s` image pixels per unit, so the overlay keeps the design's
 * proportions at any window size and stays sharp. Colours are 0xRRGGBB, drawn over what is there with opacity
 * `a` (0..1), blended as browsers do (on the stored values). The image is kept premultiplied: drawn on a
 * transparent image (alpha 0) the result is a see-through picture to blend onto another (the HUD). */
#ifndef YW4_UI_H
#define YW4_UI_H

#include <stdint.h>

typedef struct {
    uint8_t *px;  /* RGBA, rows of w */
    int w, h;
    float s;      /* image pixels per design unit */
    float ox, oy; /* where the design's (0, 0) is, in image pixels */
} UiCanvas;

enum {
    UI_ROUNDED_500, UI_ROUNDED_700, UI_ROUNDED_800,  /* M PLUS Rounded 1c: titles, buttons */
    UI_GOTHIC_400, UI_GOTHIC_500, UI_GOTHIC_700,     /* Zen Kaku Gothic New: body text */
    UI_MONO,                                         /* the system's monospaced font: the HUD's digits */
    UI_FONTS
};

/* A rectangle, and a rounded box (radius r) with a border of width bw (inside the box, as CSS's border-box;
 * bw 0: none). */
void ui_rect(const UiCanvas *c, float x, float y, float w, float h, uint32_t rgb, float a);
void ui_box(const UiCanvas *c, float x, float y, float w, float h, float r, uint32_t fill, float bw, uint32_t border);
/* A rounded box of colour rgb at opacity a (no border), and a soft shadow around a box: CSS
 * box-shadow 0 0 <blur> rgb/a, drawn as layers (offset it by moving y). */
/* A rounded box whose fill and border have their own opacity (the HUD's see-through card). */
void ui_card(const UiCanvas *c, float x, float y, float w, float h, float r, uint32_t fill, float fa, float bw,
             uint32_t border, float ba);
void ui_box_alpha(const UiCanvas *c, float x, float y, float w, float h, float r, uint32_t rgb, float a);
void ui_shadow(const UiCanvas *c, float x, float y, float w, float h, float r, float blur, uint32_t rgb, float a);
/* Text in font f at size px (the CSS font-size), its line box of height lh (0: the font's normal height) with its
 * top at y, starting at x; returns its width. ui_text_w measures without drawing. */
float ui_text(const UiCanvas *c, int f, float px, float x, float y, float lh, const char *str, uint32_t rgb);
float ui_text_w(int f, float px, const char *str);
/* Text centred vertically in [y, y + h], from x (align 0), centred in [x, x + w] (1) or ending at x + w (2). */
void ui_text_in(const UiCanvas *c, int f, float px, float x, float y, float w, float h, int align, const char *str,
                uint32_t rgb);
/* Text broken into lines at spaces to fit w (design units), each lh high, with letter spacing track_em (CSS
 * letter-spacing in ems), aligned as ui_text_in; returns its height. c NULL: only measures. */
float ui_text_wrap(const UiCanvas *c, int f, float px, float track_em, float x, float y, float w, float lh, int align,
                   const char *str, uint32_t rgb);
/* An RGBA image (iw x ih) drawn into the box at (x, y), w x h units, its corners rounded by r. */
void ui_image(const UiCanvas *c, const uint8_t *rgba, int iw, int ih, float x, float y, float w, float h, float r);
/* A line icon: SVG elements (path, circle...) of a 24 x 24 view box drawn `size` units square at (x, y), stroked
 * with width `stroke` (in the view box's units), round caps and joins, in rgb. */
void ui_icon(const UiCanvas *c, const char *svg_elements, float x, float y, float size, float stroke, uint32_t rgb);

#endif
