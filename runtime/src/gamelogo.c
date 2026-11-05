/* The game's logo for the in-game overlay's header: the game's own icon, which recomp.build puts into yw4.exe as
 * icon resource 1 when tools.yw4.icon made it from the player's files (decision 0048; game data, never in the
 * repo). Read back from the program's resources at 256 x 256 as RGBA; NULL without one (no icon was built in,
 * or not Windows): the overlay then draws its clock. */
#include <stdint.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

const uint8_t *game_logo(int *w, int *h);

const uint8_t *game_logo(int *w, int *h) {
    static uint8_t *px;
    static int tried, lw, lh;
#ifdef _WIN32
    if (!tried) {
        tried = 1;
        HICON ic = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON, 256, 256, LR_DEFAULTCOLOR);
        ICONINFO ii;
        if (ic && GetIconInfo(ic, &ii)) {
            BITMAP bm;
            if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof bm, &bm) && bm.bmWidth > 0 && bm.bmHeight > 0) {
                BITMAPINFO bi = {.bmiHeader = {.biSize = sizeof(BITMAPINFOHEADER), .biWidth = bm.bmWidth,
                                               .biHeight = -bm.bmHeight, .biPlanes = 1, .biBitCount = 32,
                                               .biCompression = BI_RGB}};
                uint8_t *p = malloc((size_t)bm.bmWidth * (size_t)bm.bmHeight * 4);
                HDC dc = GetDC(NULL);
                if (p && GetDIBits(dc, ii.hbmColor, 0, (UINT)bm.bmHeight, p, &bi, DIB_RGB_COLORS)) {
                    size_t n = (size_t)bm.bmWidth * (size_t)bm.bmHeight;
                    int any_alpha = 0;
                    for (size_t i = 0; i < n; i++) any_alpha |= p[i * 4 + 3];
                    for (size_t i = 0; i < n; i++) {  /* BGRA -> RGBA; an icon without alpha is opaque */
                        uint8_t t = p[i * 4];
                        p[i * 4] = p[i * 4 + 2], p[i * 4 + 2] = t;
                        if (!any_alpha) p[i * 4 + 3] = 255;
                    }
                    px = p, lw = bm.bmWidth, lh = bm.bmHeight;
                } else {
                    free(p);
                }
                ReleaseDC(NULL, dc);
            }
            if (ii.hbmColor) DeleteObject(ii.hbmColor);
            if (ii.hbmMask) DeleteObject(ii.hbmMask);
        }
        if (ic) DestroyIcon(ic);
    }
#else
    (void)tried;
#endif
    *w = lw, *h = lh;
    return px;
}
