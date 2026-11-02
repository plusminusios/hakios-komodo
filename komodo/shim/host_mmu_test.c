/* host_mmu_test.c — verify the MMU descriptor/attribute logic offline.
 * Build with -DMMU_HOST (drops the sysreg enable path). */
#include <stdio.h>
#include "mmu.h"

#define DESC_BLOCK (1ULL<<0)
#define DESC_AF    (1ULL<<10)
#define ATTR(d)    (((d)>>2)&7)
#define SH(d)      (((d)>>8)&3)

static int fail;
static void chk(const char *what, int cond)
{
    printf("  %-48s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) fail = 1;
}

int main(void)
{
    /* Peripheral GB (GICD 0x10400000 is in GB 0) -> Device, non-shareable. */
    uint64_t gic = mmu_block_desc(0x10400000ULL);
    chk("GIC block: block bit",      gic & DESC_BLOCK);
    chk("GIC block: access flag",    gic & DESC_AF);
    chk("GIC block: attr=DEVICE(0)", ATTR(gic) == MAIR_IDX_DEVICE);
    chk("GIC block: SH=non-shareable", SH(gic) == 0);
    chk("GIC block: addr = GB0 (0)", (gic & 0xFFFFC0000000ULL) == 0x0ULL);

    /* UFS 0x13200000 still GB0 device. */
    chk("UFS attr=DEVICE", ATTR(mmu_block_desc(0x13200000ULL)) == MAIR_IDX_DEVICE);

    /* DRAM base 0x80000000 (GB2) -> Normal, inner-shareable. */
    uint64_t ram = mmu_block_desc(0x80000000ULL);
    chk("DRAM block: attr=NORMAL(1)", ATTR(ram) == MAIR_IDX_NORMAL);
    chk("DRAM block: SH=inner(3)",    SH(ram) == 3);
    chk("DRAM block: addr = 0x80000000", (ram & 0xFFFFC0000000ULL) == 0x80000000ULL);

    /* High DRAM 0x1_00000000 (GB4) -> Normal. */
    chk("high DRAM attr=NORMAL", ATTR(mmu_block_desc(0x100000000ULL)) == MAIR_IDX_NORMAL);

    /* fb fallback 0xf0000000 (3.75 GiB) is inside low DRAM -> Normal by default. */
    chk("fb fallback 0xf0000000 attr=NORMAL", ATTR(mmu_block_desc(0xf0000000ULL)) == MAIR_IDX_NORMAL);

    /* Build the whole table and spot-check a couple of GB indices. */
    mmu_build_tables();
    uint64_t *t = mmu_l1_table();
    chk("table[0]  = GB0 device",  ATTR(t[0]) == MAIR_IDX_DEVICE);
    chk("table[2]  = GB2 normal (0x80000000)", ATTR(t[2]) == MAIR_IDX_NORMAL && (t[2] & 0xFFFFC0000000ULL) == 0x80000000ULL);
    chk("table[4]  = GB4 normal",  ATTR(t[4]) == MAIR_IDX_NORMAL);
    chk("table[511]= GB511 normal",ATTR(t[511]) == MAIR_IDX_NORMAL);

    /* After mapping the fb (0xf0000000 -> GB3) as NC, that block is Normal-NC. */
    mmu_map_fb_nc(0xf0000000ULL);
    chk("fb GB3 remapped Normal-NC", ATTR(t[3]) == MAIR_IDX_NC && SH(t[3]) == 0);
    chk("neighbour GB2 still normal", ATTR(t[2]) == MAIR_IDX_NORMAL);

    printf("RESULT: %s\n", fail ? "FAIL" : "PASS");
    return fail;
}
