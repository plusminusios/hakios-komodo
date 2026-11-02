/* console.h — komodo bare-metal framebuffer console (no-cable debug channel).
 *
 * The Pixel 9 Pro XL bootloader framebuffer is a RUNTIME-allocated region
 * (reserved-memory/vframe = shared-dma-pool CMA): there is no fixed address and
 * no framebuffer node in the static FDT. So the shim must learn the fb geometry
 * at boot (from the live FDT the ABL passes, or a first-hardware-session value).
 * Until that is confirmed on hardware, con_init falls back to the native panel.
 *
 * Native panel (verified on device): 1344 x 2992, a8r8g8b8, stride 1344*4.
 */
#ifndef KOMODO_CONSOLE_H
#define KOMODO_CONSOLE_H

#include <stdint.h>

#define KOMODO_FB_W       1344
#define KOMODO_FB_H       2992
#define KOMODO_FB_STRIDE  (KOMODO_FB_W * 4)     /* bytes */
/* Fallback fb base if the live FDT yields nothing (first-session guess; the
 * real address is CMA-allocated by ABL and must be confirmed on hardware). */
#define KOMODO_FB_FALLBACK 0xf0000000UL

/* 32-bit a8r8g8b8 colors. */
#define C_BLACK   0xff000000u
#define C_WHITE   0xffffffffu
#define C_RED     0xffff0000u
#define C_GREEN   0xff00ff00u
#define C_BLUE    0xff0000ffu
#define C_YELLOW  0xffffff00u
#define C_CYAN    0xff00ffffu

/* Bind the console to a framebuffer. Returns 1 on success. */
int  con_init(uint64_t fb_base, uint32_t w, uint32_t h, uint32_t stride);

/* Fill the whole screen with one color (fastest possible "I am alive" signal). */
void con_fill(uint32_t color);

/* Draw `n` thick horizontal bars from the top = boot stage reached. Font-free,
 * readable from a phone photo: count the bars to see how far the shim got. */
void con_stage(int n, uint32_t color);

/* Print a labelled 64-bit hex value as one row of glyphs (compact hex font).
 * `tag` is 0..3 leading marker glyphs (indices into the font: see console.c).
 * Used to show register/exception values with no full font. */
void con_hexrow(int row, const char *label, uint64_t value, uint32_t color);

/* Lowest-level: put one 8x8 glyph (font index) at pixel (px,py), scaled. */
void con_glyph(int gidx, int px, int py, int scale, uint32_t color);

/* Auto-advancing log: con_logreset() resets to the top; each con_log() prints
 * one labelled hex value on the next row (wraps at the screen bottom). */
void con_logreset(void);
void con_log(const char *label, uint64_t value, uint32_t color);

/* Draw a text string (uses the built-in font) at pixel (px,py), scaled. */
void con_str(const char *s, int px, int py, int scale, uint32_t color);

/* Print a string on the next auto-advancing log row (for status/errors). */
void con_logstr(const char *s, uint32_t color);

#endif
