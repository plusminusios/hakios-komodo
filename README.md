# hakios-komodo

*English · [Русский](README.ru.md)*

A work-in-progress port of the Apple XNU / Darwin kernel (iOS 16, `xnu-8796.141.3`)
to the Google Pixel 9 Pro XL (codename **komodo**, board **zumapro**, SoC **Google
Tensor G4**).

The project builds on the ios-tegu / ios-sweet / HakiOS line of work and reuses the
`MACHINE_CONFIG=TEGU` machine slot, since komodo belongs to the same Tensor hardware
family as the Pixel 9a ("tegu"). All testing is performed entirely in RAM via
`fastboot boot`; nothing is written to flash and the stock Android installation is
left intact.

A full technical write-up is kept in [STATUS-komodo.md](STATUS-komodo.md)
(short shareable copy: [gist](https://gist.github.com/plusminusios/25461cd8e515665a7a7b350ff903e6de)).

## Current status

### Working (verified on device)

- **Toolchain.** XNU is built on Linux without macOS, using LLVM 18.1.8, the theos
  cctools `ld64`, and the macOS 13.3 SDK. A full kernel (`kernel.development.tegu`,
  Mach-O arm64 PIE) builds in roughly two minutes.
- **Boot chain.** An arm64 `Image` shim (header flags `0xa`, AVB footer required by
  the komodo ABL) drops from EL2 to EL1, loads the embedded `KOMOXNU` payload and
  enters XNU.
- **Early kernel.** Darwin 22.6.0 reaches `arm_init`, brings up the MMU and reaches
  `kprintf`.
- **Display.** AppleCLCD framebuffer at 1344x2992, base taken from `boot_args` / DPU.
- **Memory.** The 14 GB high DRAM bank at `0x880000000` is usable; the XNU window is
  placed on a clean region at `0x98000000`.
- **IOKit.** `TeguPlatformExpert`, AppleKeyStore, the Sandbox and AMFI stand-ins come
  up.
- **User space.** `init` reaches **PID 1** — a functioning Darwin user space on the
  device.

### Known blocker: UFS

The storage controller is a Samsung Exynos UFS host (UFSHCI 3.0). On hand-off the
bootloader leaves the UniPro link in hibernate / a fatal power state:

- at hand-off `HCS` reports the device present (DP=1) and `PA_PWRMode` is `0x11`
  (HS, two lanes), but `DME_PEER_GET` against the device fails (`res 0xa`);
- `UPMCRS` reads `5` (`PWR_FATAL_ERROR`) and transfer requests never complete
  (`OCS 0xf`).

The gs101 PHY hibernate-exit sequence from mainline Linux
(`ufs-exynos.c` + `phy-gs101-ufs.c`) has been ported, but the link does not resume:
writes to the PMA block at `0x13204000` do not take effect while reads succeed. The
PHY power-isolation control lives in the TrustZone-owned PMU and is not writable from
EL1. See [STATUS-komodo.md](STATUS-komodo.md) for the full analysis and open
questions.

## Repository layout

Only source is tracked; build outputs and third-party toolchains are ignored and are
fetched or produced by `run-build.sh`.

| Path | Contents |
| --- | --- |
| `xnu-linux/run-build.sh`, `xnu-linux/linux/` | macOS-free XNU build (LLVM 18, ld64, SDK 13.3) |
| `xnu-linux/patches-src/`, `komodo/ci/*.patch` | patch series (tegu base + komodo `008x..`) |
| `komodo/shim/` | boot shim (EL2 to EL1, embedded payload), `komodo-base.dtb` |
| `komodo/*.py` | `mkxnuboot`, `mkadt`, `mkpayload` |
| `komodo/ci/tegu_ufs.c` | current UFS driver (active work) |
| `komodo/research/`, `HARDWARE-MAP-komodo.md` | komodo hardware reverse-engineering notes |
| `boot-ufs.sh`, `build-ufs.sh` | build and launch helpers for the UFS tests |

## Building

```bash
cd xnu-linux && ./run-build.sh        # fetches toolchains, builds kernel.development.tegu
../build-ufs.sh 0x116 xnu-ufsP-phy    # package a RAM-only UFS test image
../boot-ufs.sh P                      # fastboot boot; read the on-screen banner
```

The device must be in fastboot mode. Each image is loaded with `fastboot boot`, so
nothing is flashed.

## Scope and limitations

Touch, buttons, USB, Wi-Fi and the GPU are disabled: the tegu drivers target
Qualcomm parts, whereas komodo is Tensor/Exynos and needs its own drivers (Synaptics
S3908TG over Samsung USI SPI, dwc3, the Mali GPU, Broadcom Wi-Fi, and the S2MPG14 PMIC
behind ACPM). None of these have been ported for Tensor yet.

This is a research port. A complete iOS user interface comparable to the Poco X5 Pro
HackiOS demo (a Snapdragon device for which those drivers already exist) is a
substantial, long-term effort. The realistic near-term goal is a booting kernel and,
at most, a static display.

## Contributing

The immediate problem is UFS link recovery. Two concrete directions:

1. Determine why writes to the PMA block (`0x13204000`) do not latch — PHY clock /
   enable gating, or a secure-world (SMC) path for isolation and calibration.
2. Implement a full cold bring-up (HCE reset, gs101 PMA `CFG_PRE_INIT`,
   `DME_LINKSTARTUP`, power-mode change) instead of resuming the bootloader's
   hibernate, which fatals consistently.

Access to a Tensor UFS PHY reference, or the downstream Google `ufs-exynos` /
`ufs-cal-if` sources for zumapro, would help considerably.

## Credits and license

XNU is licensed under APSL 2.0 (Apple). The macOS-free build harness is based on
ios-sweet-linux (APSL 2.0). The patches, scripts and drivers in this repository are
provided for research purposes.
