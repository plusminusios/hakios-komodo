/* fb.h — discover the bootloader framebuffer for the komodo shim.
 *
 * komodo has no framebuffer node in its static FDT and the region is CMA-
 * allocated (reserved-memory/vframe = shared-dma-pool), so there is no fixed
 * address to read offline. fb_discover() tries, in order:
 *   1. a "simple-framebuffer" node in the (live) FDT  [source FB_SRC_SIMPLEFB]
 *   2. the ABL/chosen framebuffer address, if present  [FB_SRC_CHOSEN]
 *   3. the compile-time fallback KOMODO_FB_FALLBACK     [FB_SRC_FALLBACK]
 * Geometry defaults to the known native panel (1344x2992) unless the node
 * carries width/height/stride. The chosen source is reported on screen so the
 * first hardware session can confirm the real address.
 */
#ifndef KOMODO_SHIM_FB_H
#define KOMODO_SHIM_FB_H

#include <stdint.h>

enum { FB_SRC_FALLBACK = 0, FB_SRC_SIMPLEFB = 1, FB_SRC_CHOSEN = 2, FB_SRC_DPP = 3 };

/* zumapro DPU registers (confirmed on komodo: drmdecon@19470000,
 * drmdpp@19900000; identical to zuma Pixel 8 Pro, panel 1344x2992):
 *   DPP0 RDMA_BASEADDR_P0 @ 0x19900040 holds the live bootloader fb address.
 *   DECON0 TRIG_CON        @ 0x19470030 — write 0x3061 to UNMASK the hardware
 *   trigger so the panel scans out the framebuffer (masked = 0x3070 = black). */
#define DPP0_RDMA_BASEADDR_P0  0x19900040UL
#define DECON0_TRIG_CON        0x19470030UL
#define DECON0_TRIG_UNMASK     0x3061U

struct fb_info {
    uint64_t base;
    uint32_t width, height, stride;
    int      source;   /* enum above */
};

/* Fill `out` from the FDT at `fdt` (may be NULL -> fallback only).
 * Returns out->source. */
int fb_discover(const void *fdt, struct fb_info *out);

/* Unmask the DECON0 hardware trigger so the panel refreshes from the fb.
 * Without this the Tensor DPU leaves the panel black after ABL hands off.
 * No-op on the host build. */
void fb_decon_trigger(void);

#endif
