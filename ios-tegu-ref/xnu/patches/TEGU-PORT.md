# TEGU-PORT.md — what the ios-sweet -> tegu port changed, and what is still open

All 81 XNU patches are the ios-sweet series (renamed sweet->tegu) with the
confirmed tegu values applied where the hardware facts are known. Register
sources: NOTES.md (from the stock CP3A.260905.009 DTBs).

## Already adapted (confirmed values)

| Area | Change |
|---|---|
| mkadt.py | GICD 0x10400000/0x10000, GICR 0x10440000/0x200000, timer 24.576 MHz, UFS 0x13200000, DRAM 0x80000000, panel 1080x2406 |
| 0010/0012 (early fb console) | height 2406; fb address is a placeholder (see below) |
| 0046/0061 (framebuffer/display) | 1080x2406, 270 ppi; iPhone X presentation logic unchanged |
| 0060/0058 (touch/HID) | panel geometry 2406 |
| VTIMER_INTID | 30 (PPI 14) — *(verify at first boot)* |
| SMP | tegu has 8 cores in 3 clusters (4+3+1); start cpus=1 then cpus=4 (the A520 cluster), patch 0078's 8-core work adapts to cpus=8 |

## Open — needs the live device (see docs/BRINGUP.md)

1. **Framebuffer address** (patches 0010/0012, mkadt.py, shim): on tegu the
   bootloader allocates the framebuffer at runtime; the placeholder 0xf0000000
   must be replaced by a value the shim reads from the live FDT and patches
   into the ADT. First phone test resolves it (live FDT dump).
2. **Touch controller**: not present in the base DTB dump; patches 0060/0063
   are sweet's Goodix-over-QCOM-GENI driver and must be re-pointed at tegu's
   actual touch IC (verify from the live FDT/dmesg; tegu's I2C/SPI buses are
   `samsung,exynos-hsi2c` at 0x10880000+ etc.).
3. **UFS glue**: patches 0028-0034/0069 are the SM7150 host driver; tegu's is
   `samsung,exynos-ufs` @ 0x13200000 with vendor regs (NOTES.md). The
   UFSHCI-standard register set carries over; clocks/PHY/sysreg differ
   (sysreg_ufs 0x13020000, PHY 0x13280000).
4. **PMIC buttons/battery**: patches 0062 use sweet's Qualcomm PMIC over
   SPMI; tegu uses Samsung S2MPG14/S2MPG15 (SPMI; rails documented in the
   dtbo overlays). Re-point the driver.
5. **USB (0072-0081)**: sweet's dwc2; tegu has a dwc3 @ 0x11210000 — the
   gadget/network bring-up will need dwc3 register work.
6. **shim/build.sh boot image**: currently mirrors ios-sweet's boot.img v2
   recipe. tegu is GKI: boot.img v4 + vendor_boot v4 (page 2048) +
   vendor_kernel_boot. The mkbootimg invocation must be re-derived from the
   stock tegu images on the first fastboot test (see docs/BRINGUP.md M2).
7. **Memory banks**: static DTB declares 2 GB at 0x80000000; the real 8 GB
   layout (banks above 4G) comes from the live FDT.

## Conventions

- Values already confirmed never get "(verify)"; unknowns do, and AGENTS.md
  forbids inventing them.
- Patch chain integrity: 0058/0061 share HID scaling lines (/ 2400); do not
  edit one without the other.
