/* Video for Mikie (Konami GX469).
 *
 * 32x32 tilemap of 8x8 tiles plus 36 sprites of 16x16, all 4bpp, through a
 * two-stage indirect palette. Output is the visible area already rotated for
 * the vertical monitor: 224 wide x 256 tall.
 */
#ifndef MIKIE_VIDEO_H
#define MIKIE_VIDEO_H

#include <stdint.h>

#define SCR_W       256           /* raster width  before rotation */
#define SCR_H       224           /* visible height before rotation */
#define OUT_W       SCR_H         /* 224, after ROT270 */
#define OUT_H       SCR_W         /* 256, after ROT270 */

/* Loads and decodes the graphics ROMs and the five PROMs.
   Returns 0 on success. `romdir` holds the files under their MAME names. */
int video_init(const char *romdir);

/* Render one frame.
     spriteram : 144 bytes from $2800
     colorram  : 1024 bytes from $3800
     videoram  : 1024 bytes from $3C00
     rgb       : OUT_W * OUT_H * 3 bytes, written rotated and ready to display */
void video_render(const uint8_t *spriteram, const uint8_t *colorram,
                  const uint8_t *videoram, int palettebank, int flip,
                  uint8_t *rgb);

#endif /* MIKIE_VIDEO_H */
