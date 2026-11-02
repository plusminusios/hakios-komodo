# tegu bring-up plan (XNU + iOS 16 userland on Pixel 9a)

Ordered milestones. Every hardware test is `fastboot boot` (temporary,
nothing flashed). Do not start milestone N+1 before N is green.

## Milestone 0 — prerequisites (owner, once)

1. **Back up the phone.** Unlocking erases all userdata.
2. Settings → About phone → tap "Numéro de build" 7x → Developer options →
   enable **OEM unlocking** (needs a non-carrier-locked device).
3. Power off, hold **Volume-Down + Power** → fastboot mode.
4. PC (Windows/Mac fine, only platform-tools needed):
   `fastboot flashing unlock` (confirm on screen; wipes the device).
5. Reboot to Android, set it up, enable USB debugging (for adb steps).

## Milestone 1 — CI builds XNU for tegu (no phone needed) — DONE 2026-10-20

- The patch series and the xnu.yml workflow are a port of ios-sweet's
  working CI. First goal: `gh workflow run xnu.yml` green, producing
  `kernel.development.tegu`.
- **Done.** `xnu` run 4 (commit 1975694) built `kernel.development.tegu`
  (17.2 MB) with `-k`; after that, `-k` is off so builds fail fast.
- Locally useful: `tools/xnu-cc.py` to replay single compiles from a CI log.
- Stock firmware reference is already extracted (see NOTES.md). If needed
  again: `tools/ota-fetch.sh` + `tools/unpack_bootv4.py` + `tools/fdt-read.py`.

## Milestone 2 — shim runs on hardware (first phone test)

- Open questions for the first test *(verify)*:
  1. The exact `fastboot boot` form on tegu: try
     `fastboot boot boot.img --vendor_boot vendor_boot.img
     --vendor_kernel_boot vendor_kernel_boot.img` (all three from the OTA
     artifacts; the stock images boot fine, so they define the accepted format).
  2. Whether the tegu ABL hands x0 = FDT pointer per the Linux protocol
     (expected; the shim sanity-checks it and reports).
  3. The framebuffer location in the live FDT (the ADT placeholder must be
     patched at boot by the shim).
- Acceptance: `fastboot boot shim-boot.img` doesn't bounce back to Android;
  evidence = fb photo, fastboot stderr, or ramoops after rebooting Android.

## Milestone 3 — XNU boots to a prompt (PSCI + GICv3 + timer)

- cpus=1 first (cpu@0), then the 4x A520 cluster (cpus=4), then 8.
- All register values are already confirmed (NOTES.md); fix runtime
  surprises from the debug loop.
- Acceptance: XNU boot log visible in the framebuffer (photo the screen).

## Milestone 4 — framebuffer console reliable

- tegufb_hz console at 30 Hz in the bootloader framebuffer; this becomes the
  primary debug channel from now on (port of sweetfb, patches 0010/0012/0046).

## Milestone 5 — UFS + HFS+ root + iOS root image

- Port the UFS driver glue to `samsung,exynos-ufs` (register list in
  NOTES.md; the UFSHCI protocol is the same as sweet's).
- Build the iPhone X 16.7.16 root image (`tools/mkiosroot.sh` +
  `tools/tegu-root-tweaks.py`; runs in a Linux CI container or on any Linux
  box). **Writing it to userdata erases Android — explicit owner approval
  required at that point.**
- Acceptance: XNU mounts the root, launchd starts (fb log lines).

## Milestone 6 — touch + buttons, SpringBoard

- Touch controller on tegu: *(verify from the live FDT / dmesg; not in the
  base DTB dump)*. Buttons via the S2MPG14/15 PMICs (SPMI).
- Acceptance: lock screen reacts to touch; unlock works; home screen shows.

## Debug loop (no serial)

1. `fastboot boot ...` from the PC.
2. Evidence, most useful first: photo of the screen (fb console),
   `tools/pstore-pull.sh` after rebooting to Android (ramoops confirmed at
   0xfd3ff000), fastboot stderr.
3. Send evidence back to the agent working the repo.

## Hardware research checklist (fills NOTES.md, needs only adb)

```
adb pull /proc/device-tree fdt-live/    # the LIVE FDT the shim will receive
adb shell cat /proc/iomem > iomem.txt
adb shell cat /proc/interrupts > interrupts.txt
adb shell cat /proc/cpuinfo > cpuinfo.txt
adb shell cat /sys/firmware/psci/version 2>/dev/null || echo no-psci > psci.txt
adb shell dmesg > dmesg.txt             # if permitted
```
Zip and share. The live FDT answers the open framebuffer question in one shot.
