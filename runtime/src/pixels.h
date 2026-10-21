/* NVN colour formats on the CPU: clear colours in, display pixels out (decision 0019). */
#ifndef PIXELS_H
#define PIXELS_H

#include <stdint.h>

/* A clear colour (linear floats, as NVN takes them) as one texel of `format`. Returns its
 * size in bytes, 0 for a format not handled yet. sRGB formats store the encoded value. */
int pixel_encode(uint32_t format, const float color[4], uint8_t out[16]);

/* One texel of `format` as RGBA8 for the screen; 0 for a format not handled yet. */
int pixel_to_rgba8(uint32_t format, const uint8_t *texel, uint8_t out[4]);

#endif
