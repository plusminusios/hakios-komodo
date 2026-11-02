/*
 * Board config for the Pixel 9 Pro XL ("komodo") and Pixel 9a ("tegu") —
 * Google Tensor G4 ("zumapro", Exynos heritage): Cortex-A520 + Cortex-A720 +
 * Cortex-X4, ARMv9.2.  No Apple IMPDEF system registers (generic ARM like
 * BCM2837/VMAPPLE), 16K pages, GICv3/v4 with two security states (TrustZone
 * owns Group 0, so every interrupt including the timer is a Group 1 IRQ taken
 * through sleh_irq()).  We do NOT use pointer authentication (plain arm64).
 *
 * This is a drop-in replacement for pexpert/arm64/TEGU.h in the LeR33/ios-tegu
 * patch series: komodo reuses the TEGU board-config slot (ARM64_BOARD_CONFIG_TEGU).
 * The values below are CORRECTED from the stock ios-tegu header, which still
 * carried the Qualcomm SM7150 ("sweet") leftovers (VTIMER_INTID 19, timer PPIs
 * 1/2/3/0, GICR_PE_SIZE 0x20000).  Every value here is verified against the
 * live Pixel 9 Pro XL (/proc/iomem, /proc/device-tree) — see komodo/README.md.
 *
 * zumapro timer PPIs are the STANDARD ARM ones (13 sec-phys, 14 ns-phys,
 * 11 virt, 10 hyp-virt) -> virtual-timer INTID 16+11 = 27 (NOT 19).
 */

#ifndef _PEXPERT_ARM64_TEGU_H
#define _PEXPERT_ARM64_TEGU_H

#define NO_MONITOR                1
#define NO_ECORE                  1

#define TEGU                     1       /* keep the macro name: shared slot */

#define ARM_ARCH_TIMER
#define __ARM_ARCH__              8       /* compile as base ARMv8 (ARMv9.2 is a superset) */
#define __ARM_VMSA__              8
#define __ARM_VFP__               4
#define __ARM_COHERENT_CACHE__    1
#define __ARM_DEBUG__             7
#define __ARM_ENABLE_SWAP__       1
#define __ARM_V8_CRYPTO_EXTENSIONS__ 1
#define __ARM_PAN_AVAILABLE__     1
#define __ARM_16K_PG__            1

#define HAS_GIC_V3                1

#ifndef ASSEMBLER
#define PLATFORM_PANIC_LOG_DISABLED
#endif /* ! ASSEMBLER */

/*
 * zumapro uses the ARCHITECTED timers with standard PPI numbering:
 *   PPI 13 secure-phys, 14 non-secure-phys, 11 virtual, 10 hyp-virt.
 * XNU's rtclock drives the VIRTUAL timer (CNTV) -> INTID 16 + 11 = 27, which
 * is also what QEMU virt wires up (xnu patch 0017 comment).  This compile-time
 * value is only the fallback: the authoritative INTID is read at boot from the
 * ADT GIC node "virtual-timer-intid" (= 27 in mkadt_komodo.py).
 * If the timer never fires on first boot, the alternative to try is 30 (the
 * non-secure PHYSICAL timer, PPI 14); that was the stock ios-tegu mkadt value.
 */
#define TEGU_VTIMER_INTID        27      /* was 19 (sweet/Qualcomm); zumapro virt PPI 11 */
#define TEGU_IPI_SGI             1       /* SGI used for cross-CPU signals */

#define GIC_SPURIOUS_IRQ          1023

/*
 * Per-redistributor stride.  zumapro's GICR region is 0x200000 for 8 PEs =
 * 0x40000 each -> GICv4 (4 x 64K frames: RD, SGI, VLPI, reserved), not the
 * GICv3 0x20000 the sweet header assumed.  Verified: live /proc/iomem GICR
 * 0x10440000 size 0x200000, and our offline FDT scan (phase3).
 */
#define GICR_PE_SIZE              0x40000 /* was 0x20000 (GICv3); zumapro is GICv4 */

/* GICv3 register definitions; see GICv3 spec (Arm IHI 0069G) */
#define GICD_CTLR                 0x0

/* Non-secure view of GICD_CTLR (DS=0) */
#define GICD_CTLR_ENABLEGRP1      0x1
#define GICD_CTLR_ENABLEGRP1A     0x2
#define GICD_CTLR_ARE_NS          0x10
#define GICD_CTLR_RWP             0x80000000

#define GICR_TYPER                              0x08
#define GICR_WAKER                              0x14
#define GICR_IGROUPR0                           0x10080
#define GICR_ISENABLER0                         0x10100
#define GICR_IPRIORITYR0                        0x10400

#define GICR_TYPER_AFFINITY_VALUE_SHIFT         32
#define GICR_TYPER_LAST                         0x10

#define GICR_WAKER_PROCESSORSLEEP               0x2
#define GICR_WAKER_CHILDRENASLEEP               0x4

#define ICC_CTLR_EOIMODE                        0x2

#define ICC_SRE_SRE                             0x1
/* End of GICv3 register definitions */

#endif /* ! _PEXPERT_ARM64_TEGU_H */
