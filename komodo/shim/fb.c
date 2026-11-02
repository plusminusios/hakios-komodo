/* fb.c — framebuffer discovery (freestanding). See fb.h. */
#include "fb.h"
#include "fdt.h"
#include "console.h"

int fb_discover(const void *fdt, struct fb_info *out)
{
    /* Default geometry: the verified native komodo panel. */
    out->base   = 0;
    out->width  = KOMODO_FB_W;
    out->height = KOMODO_FB_H;
    out->stride = KOMODO_FB_STRIDE;
    out->source = FB_SRC_FALLBACK;

    if (fdt && fdt_valid(fdt)) {
        uint64_t base = 0, size = 0;
        /* 1. simple-framebuffer (some bootloaders inject one at runtime). */
        if (fdt_find_reg_by_compatible(fdt, "simple-framebuffer", &base, &size) == 0
            && base) {
            out->base = base;
            out->source = FB_SRC_SIMPLEFB;
            /* keep native geometry unless a real driver reads the node props */
            return out->source;
        }
    }

#ifndef SHIM_HOST
    /* 2. Ask the DPU directly: DPP0 RDMA_BASEADDR_P0 holds the live fb address
     * the bootloader programmed (zuma/zumapro). This is the robust source on
     * komodo (no fb node in the FDT; the region is CMA-allocated at runtime). */
    {
        uint32_t dpp = *(volatile uint32_t *)DPP0_RDMA_BASEADDR_P0;
        if (dpp >= 0x80000000u) {   /* plausible DRAM address */
            out->base = dpp;
            out->source = FB_SRC_DPP;
            return out->source;
        }
    }
#endif

    /* 3. Fallback: best-guess address, native geometry. */
    out->base = KOMODO_FB_FALLBACK;
    out->source = FB_SRC_FALLBACK;
    return out->source;
}

void fb_decon_trigger(void)
{
#ifndef SHIM_HOST
    /* Unmask DECON0's hardware trigger so the panel scans out the fb.
     * Masked value is 0x3070 (black); 0x3061 enables TE-driven refresh (as on
     * gs201/zuma). Write + barrier. */
    *(volatile uint32_t *)DECON0_TRIG_CON = DECON0_TRIG_UNMASK;
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}
