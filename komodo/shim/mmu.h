/* mmu.h — identity-mapped MMU for the komodo shim.
 *
 * The console path works with the MMU OFF (ABL leaves it off and all accesses
 * are Device-ordered). Enabling the MMU buys cacheable RAM (speed) and is the
 * state XNU expects at handoff. Identity map (VA = PA), 4 KiB granule, 39-bit
 * VA (512 GiB) with 1 GiB L1 block descriptors:
 *   PA < 0x80000000  -> Device-nGnRnE (GIC/UFS/USB/UART/display MMIO)
 *   PA >= 0x80000000 -> Normal write-back cacheable (DRAM)
 */
#ifndef KOMODO_SHIM_MMU_H
#define KOMODO_SHIM_MMU_H

#include <stdint.h>

#define MMU_DRAM_BASE   0x80000000ULL

/* MAIR attribute indices. */
#define MAIR_IDX_DEVICE 0   /* 0x00 Device-nGnRnE          */
#define MAIR_IDX_NORMAL 1   /* 0xFF Normal WB RW-Allocate  */
#define MAIR_IDX_NC     2   /* 0x44 Normal Non-Cacheable   */

/* Build the L1 (1 GiB) block descriptor for identity-mapping physical `pa`.
 * Pure function (no hardware) so the attribute logic is host-testable. */
uint64_t mmu_block_desc(uint64_t pa);

/* Populate the static L1 table (512 x 1 GiB) covering [0, 512 GiB). */
void mmu_build_tables(void);

/* Remap the 1 GiB block covering `fb_base` as Normal Non-Cacheable, so writes
 * to a framebuffer that lives in DRAM reach the display without cache flushes.
 * Call after mmu_build_tables() / fb_discover(), before mmu_enable(). */
void mmu_map_fb_nc(uint64_t fb_base);

/* Enable the MMU + caches at the current EL (EL1 or EL2). Calls
 * mmu_build_tables() first. No-op semantics if already on. */
void mmu_enable(void);

/* Address of the L1 table (for TTBR / inspection). */
uint64_t *mmu_l1_table(void);

#endif
