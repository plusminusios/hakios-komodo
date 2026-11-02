# Hardware research notes (Pixel 9a / tegu)

Confirmed facts come from the real stock firmware, build **CP3A.260905.009**
(Android 17, September 2026): the base device trees extracted from the OTA's
`vendor_kernel_boot` partition, plus the vendor_boot cmdline. Source of truth
for everything below: `tools/fdt-read.py` on the dumped DTBs
(re-extract with `tools/ota-fetch.sh` + `tools/unpack_bootv4.py`).
Unverified items are marked *(verify)* — usually from the live device
(`/proc/device-tree` differs from the static tree: runtime-added nodes).

## SoC: Google Tensor G4 ("zumapro", Exynos heritage)

- Root compatible: `google,zumapro`. Serial, pinctrl, PMU blocks are all
  `samsung,exynos-*` — Tensor's peripheral heritage is Exynos, not Qualcomm.
- 8 CPUs in the static tree, all `arm,armv9.2`, PSCI enable-method:
  - cpu@0..cpu@300 — capacity 0xc1: the 4x Cortex-A520 cluster (start here)
  - cpu@400..cpu@600 — capacity 0x39f: the 3x Cortex-A720 cluster
  - cpu@700 — capacity 0x400: the Cortex-X4
- PSCI: `/psci` compatible `arm,psci-1.0` ✓

## Confirmed register map (from the stock DTB)

| Block | Address / value |
|---|---|
| DRAM base | `0x80000000` (`/memory@80000000`; static tree declares 2G, real 8G is filled at runtime — *(verify banks from live dump)* ) |
| GICv3 | GICD `0x10400000`/0x10000, GICR `0x10440000`/0x200000, maint PPI 9 |
| Generic timer | `arm,armv8-timer`, `clock-frequency` **24 576 000 Hz**; PPIs 13/14/11/10 flags 0xff08; vtimer PPI 14 → INTID 30 *(verify at first boot)* |
| UFS host | `samsung,exynos-ufs@13200000`, std reg 0x200; vendor regs `0x13201100`/0x2000, `0x13280000`/0x8000 (PHY), `0x132a0000`/0xa014, `0x13204000`/0x4000, `0x13208000`/0x804; **SPI 466**, level-high; `dma-coherent`; sysreg_ufs `0x13020000` |
| Console UART | `uart@10870000` `samsung,exynos-uart`, stock cmdline: `earlycon=exynos4210,mmio32,0x10870000 console=ttySAC0,115200` *(whether the UART pins are reachable on the production board: unverified)* |
| Ramoops | `ramoops_mem@fd3ff000`, 0x400000 total, console 0x200000, pmsg 0x200000 ✓ |
| Watchdogs | `watchdog_cl0@10060000`, `watchdog_cl1@10070000` (`google,zuma-cl*-wdt`) |
| USB | `usb@11210000` (dwc3), phy `0x11100000`, xhci DMA buffers `0x17BC0000` |
| Display (Exynos DPU) | decon `0x19470000`, dsim `0x19440000`, dpp `0x19900000`... (only needed if we ever drive the panel ourselves; plan is bootloader-fb, like ios-sweet) |
| PMICs | S2MPG14 + S2MPG15 (Samsung), SPMI *(details in the dtbo overlays)* |

## Panel

- 1080x2406 (6.3", ~270 ppi). Patch 0046/0061 already present it as an
  iPhone X panel with a centered 1080x2406 window (ios-sweet approach).
- Bootloader framebuffer address is **runtime** on tegu (allocated by the
  bootloader, unlike sweet's fixed 0x9c000000). The shim must read it from the
  live FDT *(verify: dump `/proc/device-tree` from Android; expect a
  framebuffer node under /chosen or /reserved-memory)*. Placeholder
  `0xf0000000` is used in mkadt.py and patches 0010/0012 until then.

## Boot chain (Android side)

- GKI, kernel 6.6: kernel in `boot.img` (header v4, kernel at offset 4096,
  16.6 MB gzip), vendor ramdisk in `vendor_boot.img` (VNDRBOOT v4,
  **page size 2048**, ramdisk lz4-legacy compressed in 2 fragments),
  overlays in `dtbo.img` (dt_table, 10 uncompressed overlays — mostly PMIC
  rails), and **`vendor_kernel_boot`** carries 4 full base DTBs
  (`google,zumapro`). `init_boot` holds the generic ramdisk.
- Stock vendor_boot cmdline (in the image): `dyndbg=... earlycon=...`
- `androidboot.boot_devices=13200000.ufs` in the ramdisk — confirms UFS path.
- *(candidate, issue #2)* exact `fastboot boot` invocation on tegu:

      fastboot boot shim-boot.img --vendor_boot vendor_boot.img \
          --vendor_kernel_boot vendor_kernel_boot.img

  with stock vendor images from tools/ota-fetch.sh; only boot.img is ours.

### Stock image headers (decoded from CP3A.260905.009, issue #3)

- `boot.img`: ANDROID! v4, page 4096, kernel first (16.6 MB gzip), ramdisk
  empty (GKI: generic ramdisk lives in init_boot), header_size 1584,
  os_version 0 (init_boot carries 0x220001a9 = 17.0.0 / 2026-09), no cmdline.
- `vendor_boot.img`: VNDRBOOT v4, page 2048, kernel_addr 0x10008000,
  ramdisk_addr 0x11000000, vendor ramdisk 40.7 MB, cmdline
  `dyndbg=... earlycon=exynos4210,0x10870000 console=ttySAC0,115200 ...`
  (confirms the Tensor debug UART at 0x10870000 — not exposed on the device).
- `vendor_kernel_boot.img`: VNDRBOOT v4, 6.5 MB ramdisk = the 4 base DTBs.
- `shim/build.sh` now emits a boot v4 mirroring these (mkbootimg vendored in
  tools/, AOSP Apache-2.0, android-12.1.0_r1). Still TEGU-TODO(#3): never
  accepted by tegu ABL yet.

### M2 pre-hardware session (Oct 2026, no phone needed)

- Confirmed from the real FDT (`vendor_kernel_boot-00.dtb`): GICD 0x10400000
  (size 0x10000), GICR 0x10440000 (size 0x200000); /timer PPIs: 13 sec-phys,
  14 ns-phys, 11 virt, 10 hyp-virt → INTIDs 30/27; clock 24.576 MHz.
  shim.c now uses these (was Qualcomm sweet values: 0x17a00000, INTID 19/18).
- **No splash/framebuffer node anywhere in the static tegu DTBs** (checked
  all 4). sweet's DTB had `cont_splash_region`; tegu's does not, so the fb
  address must come from the live FDT (ABL-injected?) or the first hardware
  session. The shim now scans the live FDT for splash/framebuffer nodes and
  falls back to 0x9c000000, reporting the source on screen (issue #5).
  → the `/proc/device-tree` adb dump is now REQUIRED to answer this.
- start.S saves raw x0-x3 to `boot_regs` (.data); shim_main prints them and
  verifies the FDT magic at x0 (issue #4).
- loader.c no longer assumes the payload sits at the ramdisk start: GKI
  concatenates boot + vendor ramdisks, so it scans the first 4 MiB for the
  TEGUXNU magic.
- reset(): Qualcomm PSHOLD/imem writes removed (unmapped on tegu → would
  fault before PSCI SYSTEM_RESET); PSCI only.
- Host-side: console renders with FB_H corrected to 2406 (panel is
  1080x2406, not 2400).
- Patch series verified out-of-band (Linux, no CI): all 81 patches apply
  cleanly against the official apple-oss-distributions/xnu xnu-8796.141.3
  tarball, with the stage scripts (0019/0020/0026) run first as dxb does.
  0031 and 0041 need the staged bsd/tegu_hfs sources to exist first.

## Serial / logging

- No exposed debug UART assumed. Debug loop: framebuffer photo +
  ramoops/pstore pull (`tools/tegu-log-pull.sh` equivalent) +
  fastboot stderr. The ramoops region is confirmed at 0xfd3ff000.

## iOS side (same as ios-sweet)

- XNU xnu-8796 + pinned libpthread/hfs/BearSSL; iPhone X (iPhone10,3) iOS
  16.7.16 (20H392) root; identity D22AP. `mkadt.py` now carries the confirmed
  tegu register values above; `--qemu` values unchanged (QEMU virt).

## Reference

- `docs/reference-sweet-notes.md` — the ios-sweet bring-up history and
  hardware notes (SM7150), kept as the primary reference for how every
  subsystem was solved. This project is a port of
  https://github.com/dipa-studio/ios-sweet to the Pixel 9a.

- 2026-10-20: kernel.development.tegu builds green on macOS-14 runner (17.2 MB, CI run 4, -k now off by default).
- 2026-10-20: CI has a QEMU smoke job (evidence mode, non-blocking) booting the
  fresh kernel + ADT (--qemu) in virt for 45 s and archiving fb.png/fb.txt/
  serial.txt/regs.txt/qemu.log. First successful run (after two tool bugs
  found by the job itself: mkadt QEMU-mode reg pair went negative with a fixed
  tegu SOC_BASE, now soc_base = gicd in both modes; qemu-tegu's magic assert
  compared a NUL-padded 8s read against the 7-byte literal). Baseline verdict:
  blank screen, no serial (tegu UART is not emulated; no PL011 node in ADT),
  exit 3. The kernel executes real instructions then takes:
    Data Abort EL1 ESR 0x96000010 FAR 0xfffffe000c1e4000 (write ~97 MB above
    virt_base, past kernel_end 0xfffffe00084c8000) ELR 0xfffffe0007a78fcc,
    then the vector path itself faults (Undefined Instruction ESR 0x2000000),
    PC loops at the vectors 0xfffffe0007487000.
  So: code runs, static mapping ends before the fault target, and the
  exception vectors are not usable yet at that stage. First QEMU baseline;
  tracked in the QEMU-boot issue. Also QEMU_GICD corrected to 0x08010000
  (virt gic-v3 distributor, was 0x08000000).
