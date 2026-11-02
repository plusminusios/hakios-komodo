/* mmu.c — identity-mapped MMU for the komodo shim (freestanding). See mmu.h. */
#include "mmu.h"

/* One L1 table: 512 entries x 1 GiB = 512 GiB of identity map, 4 KiB granule.
 * 4 KiB aligned as required for a translation table base. */
static uint64_t l1_table[512] __attribute__((aligned(4096)));

/* Block (1 GiB) descriptor bit fields (AArch64 VMSAv8-64, stage 1). */
#define DESC_BLOCK      (1ULL << 0)          /* bits[1:0]=01 : block at L1/L2 */
#define DESC_AF         (1ULL << 10)         /* Access Flag                   */
#define DESC_SH_NONE    (0ULL << 8)          /* non-shareable (device)        */
#define DESC_SH_INNER   (3ULL << 8)          /* inner-shareable (normal)      */
#define DESC_ATTR(idx)  (((uint64_t)(idx) & 7) << 2)
/* AP[2:1]=00 : EL1 read/write. 1 GiB block address occupies bits[47:30]. */
#define BLOCK_ADDR_MASK 0x0000FFFFC0000000ULL

uint64_t mmu_block_desc(uint64_t pa)
{
    uint64_t d = (pa & BLOCK_ADDR_MASK) | DESC_BLOCK | DESC_AF;
    if (pa >= MMU_DRAM_BASE)
        d |= DESC_ATTR(MAIR_IDX_NORMAL) | DESC_SH_INNER;
    else
        d |= DESC_ATTR(MAIR_IDX_DEVICE) | DESC_SH_NONE;
    return d;
}

uint64_t *mmu_l1_table(void) { return l1_table; }

void mmu_build_tables(void)
{
    for (uint64_t i = 0; i < 512; i++)
        l1_table[i] = mmu_block_desc(i << 30);  /* i * 1 GiB */
}

void mmu_map_fb_nc(uint64_t fb_base)
{
    uint64_t gb = fb_base >> 30;          /* which 1 GiB block */
    if (gb >= 512)
        return;
    uint64_t pa = gb << 30;
    l1_table[gb] = (pa & BLOCK_ADDR_MASK) | DESC_BLOCK | DESC_AF |
                   DESC_ATTR(MAIR_IDX_NC) | DESC_SH_NONE;
}

#if !defined(MMU_HOST)
/* --- hardware enable path (cross-compile only; needs real sysregs) --- */

#define MAIR_VALUE  ( (0x00ULL << (8 * MAIR_IDX_DEVICE)) | \
                      (0xFFULL << (8 * MAIR_IDX_NORMAL)) | \
                      (0x44ULL << (8 * MAIR_IDX_NC)) )

/* TCR_EL1/EL2: T0SZ=25 (39-bit VA, 512 GiB), 4 KiB granule (TG0=0),
 * inner-shareable, WB cacheable walks, IPS/PS = 40-bit (0b010). */
#define TCR_T0SZ        (25ULL << 0)
#define TCR_IRGN0_WB    (1ULL << 8)
#define TCR_ORGN0_WB    (1ULL << 10)
#define TCR_SH0_INNER   (3ULL << 12)
#define TCR_TG0_4K      (0ULL << 14)
#define TCR_EL1_IPS_40  (2ULL << 32)
#define TCR_EL2_PS_40   (2ULL << 16)

#define SCTLR_M         (1ULL << 0)
#define SCTLR_C         (1ULL << 2)
#define SCTLR_I         (1ULL << 12)

static inline uint32_t current_el(void)
{
    uint64_t el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    return (uint32_t)(el >> 2) & 3;
}

void mmu_enable(void)
{
    mmu_build_tables();
    uint64_t ttbr = (uint64_t)(uintptr_t)l1_table;
    uint64_t tcr_common = TCR_T0SZ | TCR_IRGN0_WB | TCR_ORGN0_WB |
                          TCR_SH0_INNER | TCR_TG0_4K;

    __asm__ volatile("dsb sy; isb");

    if (current_el() == 2) {
        __asm__ volatile(
            "msr mair_el2, %0\n"
            "msr ttbr0_el2, %1\n"
            "msr tcr_el2, %2\n"
            "isb\n"
            :: "r"(MAIR_VALUE), "r"(ttbr), "r"(tcr_common | TCR_EL2_PS_40) : "memory");
        __asm__ volatile(
            "tlbi alle2\n dsb sy\n isb\n"
            "mrs x0, sctlr_el2\n"
            "orr x0, x0, %0\n"
            "msr sctlr_el2, x0\n"
            "isb\n"
            :: "r"(SCTLR_M | SCTLR_C | SCTLR_I) : "x0", "memory");
    } else {
        __asm__ volatile(
            "msr mair_el1, %0\n"
            "msr ttbr0_el1, %1\n"
            "msr tcr_el1, %2\n"
            "isb\n"
            :: "r"(MAIR_VALUE), "r"(ttbr), "r"(tcr_common | TCR_EL1_IPS_40) : "memory");
        __asm__ volatile(
            "tlbi vmalle1\n dsb sy\n isb\n"
            "mrs x0, sctlr_el1\n"
            "orr x0, x0, %0\n"
            "msr sctlr_el1, x0\n"
            "isb\n"
            :: "r"(SCTLR_M | SCTLR_C | SCTLR_I) : "x0", "memory");
    }
}
#endif /* !MMU_HOST */
