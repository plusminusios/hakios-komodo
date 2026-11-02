/*
 * loader.c — act as iBoot for XNU on komodo (shim milestone M5 groundwork).
 *
 * mkpayload_komodo.py resolves the kernel's chained fixups and builds the ADT,
 * emitting a flat "KOMOXNU" payload: a header + segment table, then the segment
 * bytes and the ADT. This loader: finds the payload in the ABL ramdisk (scans
 * /chosen initrd for the magic), copies each segment to
 * phys_base + (vmaddr - virt_base), lays the ADT and the XNU boot_args behind
 * the kernel, and jumps to the entry with x0 = boot_args (MMU/caches off).
 *
 * The XNU boot_args struct is an ABI (pexpert/arm64/boot.h, Revision 2): the
 * static_asserts below pin the layout. Memory is addressed through pa2p() so the
 * copy/placement logic is host-testable against a buffer (LOADER_HOST).
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"
#include "fb.h"
#include "loader.h"

#define BOOT_LINE_LENGTH 608
#define PAGE_16K         0x4000UL

struct payload_seg {
    uint64_t vmaddr, vmsize, file_off, file_size;
};

struct payload {
    char     magic[8];               /* "KOMOXNU" */
    uint32_t version, nsegs;
    uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
    uint64_t phys_base, mem_size;
    char     cmdline[BOOT_LINE_LENGTH];
    struct payload_seg segs[];
};

/* pexpert/pexpert/arm64/boot.h, Revision/Version 2 (XNU ABI). */
struct boot_video {
    uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
    uint16_t Revision, Version;
    uint64_t virtBase, physBase, memSize, topOfKernelData;
    struct boot_video Video;
    uint32_t machineType;
    uint64_t deviceTreeP;
    uint32_t deviceTreeLength;
    char     CommandLine[BOOT_LINE_LENGTH];
    uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

/* ---- physical-address access (device = identity; host = into a buffer) ---- */
#ifdef LOADER_HOST
#include <stdint.h>
static uint8_t *g_hram; static uint64_t g_hbase, g_hsize;
void loader_set_host_ram(void *p, uint64_t base, uint64_t size) { g_hram = p; g_hbase = base; g_hsize = size; }
static void *pa2p(uint64_t pa) {
    if (pa < g_hbase || pa >= g_hbase + g_hsize) return (void *)0;  /* out of bounds */
    return g_hram + (pa - g_hbase);
}
#else
static void *pa2p(uint64_t pa) { return (void *)(uintptr_t)pa; }
#endif

static void copy_bytes(uint64_t dst_pa, uint64_t src_pa, uint64_t n)
{
    volatile uint8_t *d = pa2p(dst_pa);
    volatile const uint8_t *s = pa2p(src_pa);
    if (!d || !s) return;
    for (uint64_t i = 0; i < n; i++) d[i] = s[i];
}

static void zero_bytes(uint64_t dst_pa, uint64_t n)
{
    volatile uint8_t *d = pa2p(dst_pa);
    if (!d) return;
    for (uint64_t i = 0; i < n; i++) d[i] = 0;
}

/* mkadt_komodo.py puts this u64 in /tegu-fbcon reg[0]; the real framebuffer
 * address is only known at boot (CMA, read from DPP0). ADT property data is
 * 4-byte aligned, so compare/write as two u32 halves (MMU off: no unaligned
 * 64-bit access). Returns the number of placeholders replaced. */
#define ADT_FB_PLACEHOLDER 0x4b4f4d4f46424144ULL   /* 'KOMOFBAD' */
static int adt_patch_fb(uint64_t adt_pa, uint64_t size, uint64_t fb_base)
{
    volatile uint32_t *w = pa2p(adt_pa);
    if (!w || (adt_pa & 3)) return 0;
    int n = 0;
    for (uint64_t i = 0; i + 1 < size / 4; i++) {
        if (w[i] == (uint32_t)ADT_FB_PLACEHOLDER &&
            w[i + 1] == (uint32_t)(ADT_FB_PLACEHOLDER >> 32)) {
            w[i] = (uint32_t)fb_base;
            w[i + 1] = (uint32_t)(fb_base >> 32);
            n++;
            i++;
        }
    }
    return n;
}

static int magic_ok(const char *m)
{
    static const char want[8] = { 'K','O','M','O','X','N','U','\0' };
    for (int i = 0; i < 8; i++) if (m[i] != want[i]) return 0;
    return 1;
}

int loader_prepare(uintptr_t dtb, const struct fb_info *fb, struct loader_result *out)
{
    out->error = 0; out->entry_pa = 0; out->boot_args_pa = 0;
    out->adt_pa = 0; out->top_of_kernel_data = 0; out->nsegs = 0;
    out->adt_fb_patched = 0;

    /* Where is the payload?
     *  1. appended to this shim image, right after _end (make xnuboot): the
     *     kernel image is always loaded whole, whatever ABL does with ramdisks;
     *  2. else anywhere in the initrd (GKI concatenates the vendor ramdisk
     *     first, which is tens of MiB on komodo: scan the whole range). */
    const struct payload *p = 0;
    uint64_t scan = 0, rd_start = 0, rd_end = 0;
#ifndef LOADER_HOST
    {
        extern char _end[];
        uint64_t a = ((uint64_t)(uintptr_t)_end + 7) & ~7ULL;
        const char *m = pa2p(a);
        if (m && magic_ok(m)) { p = (const struct payload *)m; scan = a; }
    }
#endif
    if (p) {
        con_log("EMBED", scan, C_GREEN);
    } else {
        if (!fdt_chosen_u64((const void *)dtb, "linux,initrd-start", &rd_start) ||
            !fdt_chosen_u64((const void *)dtb, "linux,initrd-end", &rd_end))
            return (out->error = "no payload: not embedded, no initrd"), 1;
        con_log("INITRD", rd_start, C_WHITE);
        con_log("RDEND", rd_end, C_WHITE);
        scan = (rd_start + 7) & ~7ULL;
        while (scan + 0x2a8 <= rd_end) {
            const char *m = pa2p(scan);
            if (m && magic_ok(m)) { p = (const struct payload *)m; break; }
            scan += 8;
        }
        if (!p) return (out->error = "no KOMOXNU payload"), 1;
    }
    if (p->version != 1 || (p->adt_off & 7))
        return (out->error = "bad payload header"), 1;

    uint64_t payload_pa = scan;                 /* where the payload header sits */
    uint64_t payload_end = payload_pa + p->adt_off + p->adt_size;
    uint64_t pa_of_va = p->phys_base - p->virt_base;   /* pa = va + this */
    con_log("PHYS",  p->phys_base, C_CYAN);
    con_log("VIRT",  p->virt_base, C_CYAN);
    con_log("ENTRY", p->entry_va,  C_WHITE);

    if (payload_end > p->phys_base && payload_pa < p->phys_base + p->mem_size)
        return (out->error = "payload overlaps kernel memory"), 1;
    /* XNU owns [phys_base, +mem_size) as plain RAM: it must not cover the
     * framebuffer the panel scans out, nor this shim (we are running in it). */
    if (fb && fb->base) {
        uint64_t fb_end = fb->base + (uint64_t)fb->stride * fb->height;
        if (fb_end > p->phys_base && fb->base < p->phys_base + p->mem_size)
            return (out->error = "framebuffer overlaps kernel memory"), 1;
    }
#ifndef LOADER_HOST
    {
        extern char _start[], _end[];
        uint64_t s0 = (uint64_t)(uintptr_t)_start, s1 = (uint64_t)(uintptr_t)_end;
        con_log("SHIM", s0, C_WHITE);
        if (s1 > p->phys_base && s0 < p->phys_base + p->mem_size)
            return (out->error = "shim overlaps kernel memory"), 1;
    }
#endif

    for (uint32_t i = 0; i < p->nsegs; i++) {
        const struct payload_seg *s = &p->segs[i];
        uint64_t dst = s->vmaddr + pa_of_va;
        copy_bytes(dst, payload_pa + s->file_off, s->file_size);
        if (s->vmsize > s->file_size) {
            uint64_t z = (s->file_size + 7) & ~7ULL;
            zero_bytes(dst + z, s->vmsize - z);
        }
    }
    out->nsegs = p->nsegs;

    /* ADT then boot_args, behind the kernel. */
    uint64_t adt_pa = p->kernel_end + pa_of_va;
    copy_bytes(adt_pa, payload_pa + p->adt_off, p->adt_size);
    out->adt_fb_patched = adt_patch_fb(adt_pa, p->adt_size, fb ? fb->base : 0);
    uint64_t ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
    uint64_t top   = ba_pa + PAGE_16K;

    zero_bytes(ba_pa, PAGE_16K);
    volatile struct boot_args *ba = pa2p(ba_pa);
    if (!ba) return (out->error = "boot_args out of range"), 1;
    ba->Revision = 2;
    ba->Version = 2;
    ba->virtBase = p->virt_base;
    ba->physBase = p->phys_base;
    ba->memSize = p->mem_size;
    ba->topOfKernelData = top;
    ba->Video.v_baseAddr = fb ? fb->base : 0;
    ba->Video.v_display = 1;
    ba->Video.v_rowBytes = fb ? fb->stride : 0;
    ba->Video.v_width = fb ? fb->width : 0;
    ba->Video.v_height = fb ? fb->height : 0;
    ba->Video.v_depth = 32;
    ba->deviceTreeP = adt_pa - pa_of_va;        /* XNU reads it with MMU on: a VA */
    ba->deviceTreeLength = (uint32_t)p->adt_size;
    for (int i = 0; i < BOOT_LINE_LENGTH; i++) ba->CommandLine[i] = p->cmdline[i];
    ba->memSizeActual = p->mem_size;

    out->entry_pa = p->entry_va + pa_of_va;
    out->boot_args_pa = ba_pa;
    out->adt_pa = adt_pa;
    out->top_of_kernel_data = top;
    con_log("BOOTARG", ba_pa, C_GREEN);
    con_log("JUMP",    out->entry_pa, C_GREEN);
    return 0;
}

/* start.S: drop EL2 -> EL1 if needed, x0 = boot_args, jump. */
void xnu_enter(uint64_t entry_pa, uint64_t boot_args_pa);

const char *loader_boot(uintptr_t dtb, const struct fb_info *fb)
{
    struct loader_result r;
    if (loader_prepare(dtb, fb, &r) != 0)
        return r.error;
#ifndef LOADER_HOST
    con_log("ADTFB", (uint64_t)r.adt_fb_patched, r.adt_fb_patched ? C_GREEN : C_YELLOW);
    __asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
    xnu_enter(r.entry_pa, r.boot_args_pa);
    return "xnu returned";
#else
    return 0;
#endif
}
