/* shim.c — komodo bare-metal shim entry (freestanding).
 *
 * Boot flow (first-hardware-session scope, milestones M2-M3):
 *   start.S -> shim_main(fdt, 0)
 *     1. discover framebuffer, bring up the console   -> stage bar 1
 *     2. validate the FDT handed by ABL in x0          -> stage bar 2
 *     3. read memory / GIC / cpus from the FDT         -> stage bar 3
 *     4. print the key values as hex rows (debug channel; photo the screen)
 *   (later milestones: install vectors, GIC/timer init, load XNU+ADT, jump.)
 *
 * fb_override: 0 on hardware (discover from FDT). Non-zero lets the host test
 * point the console at a local buffer.
 */
#include <stdint.h>
#include "console.h"
#include "fb.h"
#include "fdt.h"
#include "loader.h"

/* Filled for post-mortem inspection (ramoops / a debugger / the host test). */
struct shim_report {
    uint32_t stage;
    uint64_t fdt_ptr;
    int      fdt_valid;
    uint64_t mem_base, mem_size;
    uint64_t gicd;
    int      ncpus, cpus_disabled;
    struct fb_info fb;
};

struct shim_report g_shim_report;

void exc_init(void);   /* exceptions.c: install vectors + clear record */
void wdt_disable(void); /* wdt.c: no-op unless built with WDT_OFF=1 */

/* Read-only hardware probes, ONLY when there is no payload (diag images):
 * answers for the next steps without risking an XNU boot. Each label is drawn
 * and pushed to the panel BEFORE its read, so if a read hangs the bus (clock-
 * gated block) the photo shows which one.
 *   UFS host (std UFSHCI @0x13200000): did ABL leave it enabled? (XNU's UFS
 *     driver takes the host over without a reset: needs HCE=1, HCS DP|UTRLRDY)
 *   watchdogs cl0/cl1 WTCON: armed by ABL? (bit5 = running) -> WDT_OFF=1 */
static uint32_t probe_rd(const char *label, uint64_t pa, uint32_t color)
{
    con_logstr(label, C_WHITE);
    fb_decon_trigger();
#ifndef SHIM_HOST
    uint32_t v = *(volatile uint32_t *)(uintptr_t)pa;
    __asm__ volatile("dsb sy" ::: "memory");
#else
    uint32_t v = 0; (void)pa;
#endif
    con_log(label, v, color);
    fb_decon_trigger();
    return v;
}

static void diag_probes(void)
{
    con_logstr("PROBES", C_CYAN);
    probe_rd("UFSCAP", 0x13200000UL + 0x00, C_CYAN);
    probe_rd("UFSHCS", 0x13200000UL + 0x30, C_CYAN);
    probe_rd("UFSHCE", 0x13200000UL + 0x34, C_CYAN);
    probe_rd("UTRLDBR", 0x13200000UL + 0x58, C_CYAN);
    probe_rd("WDT0CON", 0x10060000UL, C_YELLOW);
    probe_rd("WDT1CON", 0x10070000UL, C_YELLOW);
    con_logstr("PROBES DONE", C_GREEN);
    fb_decon_trigger();
}

void shim_main(const void *fdt, uint64_t fb_override)
{
    struct shim_report *r = &g_shim_report;
    r->fdt_ptr = (uint64_t)(uintptr_t)fdt;

    /* 1. framebuffer + console */
    fb_discover(fdt, &r->fb);
    uint64_t fb_base = fb_override ? fb_override : r->fb.base;
    con_init(fb_base, r->fb.width, r->fb.height, r->fb.stride);
    con_fill(C_BLACK);
    /* Install exception vectors now: from here a fault paints a red screen with
     * ESR/ELR/FAR instead of silently bouncing back to fastboot. */
    exc_init();
    r->stage = 1;

    /* 2. FDT validity */
    r->fdt_valid = (fdt && fdt_valid(fdt));
    if (r->fdt_valid)
        r->stage = 2;

    /* 3. memory / GIC / cpus from the FDT */
    if (r->fdt_valid) {
        fdt_get_memory(fdt, &r->mem_base, &r->mem_size);
        uint64_t gsz = 0;
        fdt_find_reg_by_compatible(fdt, "arm,gic-v3", &r->gicd, &gsz);
        uint64_t mpidr[16];
        r->ncpus = fdt_cpus(fdt, mpidr, 16, &r->cpus_disabled);
        r->stage = 3;
    }

    /* 4. report the key values (text log; readable from a phone photo) */
    con_logreset();
    con_log("STAGE", (uint64_t)r->stage, r->fdt_valid ? C_GREEN : C_RED);
    con_log("FDT",   r->fdt_ptr,  C_WHITE);
    con_log("MEM",   r->mem_base, C_CYAN);
    con_log("GICD",  r->gicd,     C_CYAN);
    con_log("CPUS",  (uint64_t)r->ncpus, C_WHITE);
    con_log("FB",    r->fb.base,  r->fb.source == FB_SRC_FALLBACK ? C_YELLOW : C_GREEN);

    /* Unmask the DECON trigger so the panel actually scans out what we drew
     * (Tensor leaves it masked -> black screen after ABL hands off). */
    fb_decon_trigger();

    /* 5. try to load and boot XNU from the ABL ramdisk. On success this does
     * not return (jumps into the kernel). On a missing/invalid payload it
     * returns an error, which we show and then park. */
    wdt_disable();
    const char *lerr = loader_boot(r->fdt_ptr, &r->fb);
    if (lerr) {
        con_logstr(lerr, C_RED);
        fb_decon_trigger();   /* push the error line to the panel */
        diag_probes();        /* no XNU to boot: use the time for read-only probes */
    }

    /* Milestones M4+ continue from here once this is confirmed on hardware. */
#ifndef SHIM_HOST
    for (;;) { }
#endif
}
