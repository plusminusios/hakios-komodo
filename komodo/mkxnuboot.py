#!/usr/bin/env python3
"""mkxnuboot.py — the komodo XNU boot image: shim Image + embedded KOMOXNU payload,
repacked into the STOCK boot.img container (same header/os_version/cmdline as
the image ABL is known to boot).

  mkxnuboot.py STOCK_BOOT SHIM_IMAGE PAYLOAD OUT_PREFIX
               (STOCK_BOOT may be "-" or missing: an own v4 header is used)

writes
  OUT_PREFIX-raw.img        kernel = shim + payload, plain header (flags 0xa)
  OUT_PREFIX-stocklike.img  same, but MZ code0 + LZ4-legacy compressed like stock

Why embedded: with GKI the initrd ABL builds is vendor ramdisk + (init_)boot
ramdisk, order and inclusion depend on ABL. The kernel image is always loaded
whole, so the payload rides inside it, right after the shim's _end; the shim
loader looks there first (loader.c) and only then scans the initrd.
Which variant ABL accepts is what the morning diag matrix (diag/) decides.
RAM-only: `fastboot boot OUT_PREFIX-*.img`.
"""
import os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "diag"))
import mkdiag_variants as dv  # noqa: E402  (parse_v4, repack, lz4_legacy, fix_header)
sys.path.insert(0, HERE)
import mk_bootimg_komodo as mb  # noqa: E402  (fallback header when no stock image)


def own_container():
    """boot v4 header page like mk_bootimg_komodo (no stock boot.img: CI)."""
    hdr = b"ANDROID!" + struct.pack("<III", 0, 0, mb.enc_os_version("15.0.0", "2025-05"))
    hdr += struct.pack("<I", mb.HEADER_SIZE) + struct.pack("<4I", 0, 0, 0, 0)
    hdr += struct.pack("<I", mb.HEADER_VERSION) + b"\0" * (mb.BOOT_ARGS + mb.BOOT_EXTRA)
    hdr += struct.pack("<I", 0)
    assert len(hdr) == mb.HEADER_SIZE
    return hdr + b"\0" * (dv.PAGE - len(hdr)), b"", b""


def combine(shim, payload):
    assert shim[56:60] == b"ARMd", "shim is not an arm64 Image"
    assert payload[:8] == b"KOMOXNU\0", "payload magic"
    k = bytearray(shim)
    k += b"\0" * (-len(k) & 7)                 # loader looks at (_end + 7) & ~7
    k += payload
    k += b"\0" * (-len(k) & 0xffff)            # round to 64 KiB
    struct.pack_into("<Q", k, 16, len(k))      # image_size covers the payload
    struct.pack_into("<Q", k, 24, 0xA)         # LE, 4K, place anywhere
    return bytes(k)


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    stock, shim_p, pay_p, prefix = sys.argv[1:]
    avb = None
    if stock != "-" and os.path.isfile(stock):
        st = open(stock, "rb").read()
        hdr, _kz, rd, tail = dv.parse_v4(st)
        avb = dv.stock_avb(st)   # komodo ABL refuses images without an AVB footer
    else:
        print(f"note: no stock boot image ({stock}); using an own v4 header", file=sys.stderr)
        hdr, rd, tail = own_container()
    shim = open(shim_p, "rb").read()
    kern = combine(shim, open(pay_p, "rb").read())
    tmp = prefix + ".tmp"
    outs = {
        "raw": kern,
        "stocklike": dv.lz4_legacy(dv.fix_header(kern), tmp),
    }
    for name, k in outs.items():
        path = f"{prefix}-{name}.img"
        img = dv.repack(hdr, k, rd, tail)
        if avb:
            img = dv.add_avb_footer(img, avb)
        open(path, "wb").write(img)
        print(f"{path}: kernel {len(k)} B (uncompressed {len(kern)} B)")


if __name__ == "__main__":
    main()
