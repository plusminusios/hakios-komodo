# First hardware session — checklist (M2)

Everything below was prepared without the phone; this page is the exact
runbook for the first session, in order. Each step says which issue it closes.

## 0. Preconditions (issue #1)

- [ ] `fastboot flashing unlock` done (wipes the device — back up first).
- [ ] Stock images fetched: `tools/ota-fetch.sh` (build CP3A.260905.009)
      → `vendor_boot.img`, `vendor_kernel_boot.img`, `dtbo.img`.
- [ ] PC with `fastboot` and `adb`.

## 1. Dump the live device tree (issue #5) — do this FIRST

Boot stock Android, then:

```sh
adb root 2>/dev/null || adb shell su -c true   # or any root path
adb shell "find /proc/device-tree -type f -print0 | xargs -0 cat > /dev/null" # sanity
adb pull /proc/device-tree dt-live/            # or: adb shell su -c "tar cf - /proc/device-tree" | tar xf -
```

What to look for (the static DTBs have NONE of it):

- [ ] a splash / framebuffer / cont_splash node with a `reg` pair
- [ ] the `/memory` node banks (expected: 0x80000000/2G + 0x1_00000000/0xfcc00000)
- [ ] `/reserved-memory` carve-outs (needed for `PHYS_BASE` in tools/mkadt.py)

File the dump in the issue and update `mkadt.py` constants if they differ.

## 2. Diagnostics boot (issues #2, #3, #4)

```sh
fastboot boot shim-boot.img --vendor_boot vendor_boot.img \
    --vendor_kernel_boot vendor_kernel_boot.img
```

`shim-boot.img` = diagnostics only (no xnu). If the screen lights up, it shows:

- entry registers x0-x3 (issue #4: x0 should be the FDT pointer — the shim
  also checks the 0xd00dfeed magic and says FDT OK/BAD)
- the framebuffer source (FDT splash node vs 0x9c000000 fallback — issue #5)
- MMFR0/ID regs, PSCI version

Photograph the screen for every issue. If the screen stays black:

- [ ] try `fastboot oem continue` / watch `fastboot` output for ABL errors
- [ ] re-check the invocation: GKI may want the vendor ramdisk order
      swapped — that is issue #2's open question
- [ ] pull pstore after reboot: `tools/pstore-pull.sh` (issue #6 evidence)

## 3. Payload boot (issue #6)

Only after step 2 passes:

```sh
fastboot boot shim-boot-payload.img --vendor_boot vendor_boot.img \
    --vendor_kernel_boot vendor_kernel_boot.img
```

The shim relocates, patches the ADT framebuffer, and jumps to the tegu xnu.
Anything printed on screen / in pstore goes into issue #6.

## Artifacts

From [Actions](https://github.com/LeR33/ios-tegu/actions):

- `shim-boot-tegu` (shim workflow): diagnostics image, current release asset
- `shim-boot-payload` (xnu workflow, `payload` job): kernel-bearing image
- release [v0.2.0-m2pre](https://github.com/LeR33/ios-tegu/releases/tag/v0.2.0-m2pre)

## Rules

- Never `fastboot flash` anything — `fastboot boot` only. The phone must stay
  bootable stock.
- If a boot hangs, long-press power; pstore survives the reboot:
  `tools/pstore-pull.sh`.
