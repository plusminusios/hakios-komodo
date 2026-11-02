# komodo touch — hardware spec for the XNU driver (M6)

Source: komodo MP device-tree overlay (`ota/out/dtbo.img`, entry 34 =
"ZUMA PRO KOMODO MP", board id 0x60806) + base DTB `shim/komodo-base.dtb`.
Extracted 2026-11-02 with `tools/fdt_resmem.py`-style parsing (overlays in
`research/dtbo/ov*.dtbo`).

## Controller
- **Synaptics TCM over SPI**, IC `S3908TG` (`synaptics,tcm-spi`,
  `synaptics,suffix-fw-name = S3908TG`); Google GTI/touch_offload in Android.
- SPI max 10 MHz (`spi-max-frequency = 0x989680`), `synaptics,spi-mode = 0`.
- power-on delay 200 ms, reset active 2 ms, post-reset delay 50 ms.
- display resolution in DT: 1344 × 2992 (`goog,display-resolution 0x540 0xbb0`),
  sensor channels 17 × 38 (`goog,channel-num 0x11 0x26`), 19 px/mm.

## Wiring
| signal | resource | address |
|---|---|---|
| SPI bus | `spi_20` = `/spi@111D0000`, `samsung,exynos-spi`, FIFO 64 | MMIO 0x111D0000 (0x1000), GIC SPI 408 (0x198), level |
| USI mode | `samsung,usi-phandle` (0x183) + `samsung,usi-offset 0x101c` | sysreg register selecting SPI mode for USI20 |
| SPI clocks | CMU phandle 0x2e, ids 0x238 (`ipclk_spi`), 0x233 (`gate_spi_clk`) | CMU_PERIC? — gated unless ABL used it |
| DMA | `pdma1` = PL330 @ 0x10130000 (polled PIO is enough) | |
| IRQ | `gpn0-0` (pinctrl@15060000, `google,zuma-pinctrl`), flags 0x2008 | bank offset: from the zuma pinctrl table |
| RESET | `gpp1-1` (pinctrl@10840000), active-low per `reset-on-state = 0` | |
| DVDD 1.8 V | **S2MPG14 LDO25M** (`L25M_TSP_DVDD`) | via ACPM |
| AVDD 3.3 V | **S2MPG14 LDO4M** (`L4M_TS_AVDD`) | via ACPM |

PMIC S2MPG14 (`samsung,s2mpg14mfd`, reg 0x1f) sits behind **ACPM**
(`acpm_mfd_bus@15500000`, `google,i2c-acpm`): register access is an IPC to the
APM firmware, not a plain I2C/SPMI bus. This is the hard part.

## Driver plan (in order of risk)
1. **Probe what ABL left on** (read-only, first hardware session): USI20 mode
   register, SPI clock gate bits, GPIO data for gpn0/gpp1, and whether the IC
   answers at all (TCM identify). If the rails are already up, steps 2–3 shrink
   to "toggle reset".
2. Clocks + USI mode + pinctrl (CMU/sysreg/pinctrl offsets from the Pixel
   kernel's zuma tables).
3. Power: ACPM IPC → S2MPG14 LDO25M/LDO4M enable (port of acpm_ipc + acpm_mfd).
4. SPI controller (s3c64xx register model, PIO, mode 0, ≤10 MHz).
5. Synaptics TCM protocol: identify, report config, touch reports → existing
   `tegu_hid_touch(finger, down, x, y)` in TeguHID.
6. TeguHID mapping: `iokit/Kernel/arm/hid/TeguHID.cpp:437` hard-codes the Redmi
   panel (1080 × 2406); komodo is 1344 × 2992.

Until then the Qualcomm tegu touch driver stays disabled (patch 0083).
