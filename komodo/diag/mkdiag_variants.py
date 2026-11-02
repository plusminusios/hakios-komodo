#!/usr/bin/env python3
"""mkdiag_variants.py — build the "does ABL run our code?" image matrix.

Finding (2026-11-01): `fastboot boot` of the STOCK boot.img reaches Android,
but every self-made bare-metal image (even a pure `b .` loop) bounces back to
fastboot in ~3 s. A `b .` cannot return by itself, so ABL most likely REJECTS
our image before jumping. Each variant below removes one suspected difference.
All variants reuse the stock boot.img container (same header/os_version/
cmdline/ramdisk); only the kernel changes. RAM-only: `fastboot boot <img>`.

  v0_stock_loop    stock kernel, code1 -> `b .`                (lz4 like stock)
                   HANG  => ABL jumps to kernel offset 0; format is the culprit
                   ANDROID => ABL does not enter at offset 0 (unexpected)
  v1_stock_shim    stock kernel bytes, our shim spliced at +2 MiB, code1 -> shim
                   passes any size/banner/format check ABL could do
                   EXPECT: panel shows our console (stage/FDT/FB rows) or red fault
  v2_shim_fmt      our shim alone, header fixed: MZ code0, flags=0xa (raw)
                   (the shim itself now ships flags=0xa; v2 adds the MZ code0)
  v3_shim_fmt_lz4  v2, lz4-legacy compressed like stock
  v4_shim_orig     our shim exactly as before (control: expected 3 s bounce)

Usage: mkdiag_variants.py STOCK_BOOT_IMG SHIM_IMAGE OUTDIR
"""
import os, struct, subprocess, sys

PAGE = 4096
SHIM_OFF = 0x200000          # where the shim is spliced into the stock kernel
B_SELF = 0x14000000          # `b .`
MZ_CODE0 = bytes.fromhex("4d5a40fa")   # "MZ" + harmless ccmp (as in stock)


def b_to(frm, to):
    imm = (to - frm) // 4
    assert -(1 << 25) <= imm < (1 << 25)
    return 0x14000000 | (imm & 0x3ffffff)


def pad(n):
    return (-n) % PAGE


def parse_v4(img):
    assert img[:8] == b"ANDROID!", "not an Android boot image"
    ks, rs = struct.unpack_from("<II", img, 8)
    hv = struct.unpack_from("<I", img, 40)[0]
    assert hv in (3, 4), f"boot header v{hv} unsupported"
    k_off = PAGE
    r_off = k_off + ks + pad(ks)
    tail_off = r_off + rs + pad(rs)
    return img[:PAGE], img[k_off:k_off + ks], img[r_off:r_off + rs], img[tail_off:]


def repack(hdr, kernel, ramdisk, tail):
    h = bytearray(hdr)
    struct.pack_into("<I", h, 8, len(kernel))
    out = bytes(h) + kernel + b"\0" * pad(len(kernel)) + ramdisk + b"\0" * pad(len(ramdisk))
    # v4 boot signature section (tail) is AVB data for the OLD kernel; drop it
    # (unlocked bootloader: verification is advisory). Clear signature_size.
    if struct.unpack_from("<I", hdr, 40)[0] == 4:
        out = bytearray(out)
        struct.pack_into("<I", out, 1580, 0)
        out = bytes(out)
    return out


def stock_avb(stock_img):
    """(vbmeta blob, partition size) from a stock image's AVB footer, or None."""
    if stock_img[-64:-60] != b"AVBf":
        return None
    _maj, _min, _orig, vb_off, vb_size = struct.unpack_from(">IIQQQ", stock_img, len(stock_img) - 60)
    return stock_img[vb_off:vb_off + vb_size], len(stock_img)


def add_avb_footer(img, avb):
    """Pixel ABL wants an AVB footer even when unlocked (verified 2026-11-02:
    a stock kernel repacked WITHOUT the footer bounces to fastboot). Append the
    stock vbmeta right after the image (4K aligned) and an AVBf footer at the
    end of a partition-sized image pointing at it. The hash inside vbmeta no
    longer matches: fine on an unlocked bootloader (orange state)."""
    vbmeta, part = avb
    out = bytearray(img)
    out += b"\0" * (-len(out) & 0xfff)
    vb_off = len(out)
    out += vbmeta
    if len(out) + 64 > part:
        raise SystemExit(f"image {len(out)} B does not fit the {part} B partition")
    out += b"\0" * (part - 64 - len(out))
    out += b"AVBf" + struct.pack(">IIQQQ", 1, 0, vb_off, vb_off, len(vbmeta)) + b"\0" * 28
    assert len(out) == part
    return bytes(out)


def lz4_legacy(raw, tmp):
    src, dst = tmp + ".raw", tmp + ".lz4"
    open(src, "wb").write(raw)
    subprocess.run(["lz4", "-l", "-9", "-f", "-q", src, dst], check=True)
    data = open(dst, "rb").read()
    os.remove(src); os.remove(dst)
    assert data[:4] == bytes.fromhex("02214c18")
    return data


def lz4_unpack(blob, tmp):
    if blob[:4] != bytes.fromhex("02214c18"):
        return blob
    src, dst = tmp + ".in.lz4", tmp + ".out"
    open(src, "wb").write(blob)
    subprocess.run(["lz4", "-d", "-f", "-q", src, dst], check=True)
    data = open(dst, "rb").read()
    os.remove(src); os.remove(dst)
    return data


def fix_header(img):
    k = bytearray(img)
    k[0:4] = MZ_CODE0
    k[4:8] = struct.pack("<I", b_to(4, _branch_target(img)))   # code1 jumps to _stext
    struct.pack_into("<Q", k, 24, 0xA)       # flags: LE, 4K, place anywhere
    return bytes(k)


def _branch_target(img):
    ins = struct.unpack_from("<I", img, 0)[0]
    assert ins >> 26 == 0b000101, "shim code0 is not a `b`"
    imm = ins & 0x3ffffff
    if imm & (1 << 25):
        imm -= 1 << 26
    return imm * 4               # target relative to offset 0


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    stock_path, shim_path, outdir = sys.argv[1:]
    os.makedirs(outdir, exist_ok=True)
    tmp = os.path.join(outdir, ".tmp")
    hdr, kz, rd, tail = parse_v4(open(stock_path, "rb").read())
    stock = lz4_unpack(kz, tmp)
    assert stock[56:60] == b"ARMd"
    shim = open(shim_path, "rb").read()
    assert shim[56:60] == b"ARMd"

    variants = {}
    # v0: stock, code1 -> b .
    k = bytearray(stock); struct.pack_into("<I", k, 4, B_SELF)
    variants["v0_stock_loop"] = lz4_legacy(bytes(k), tmp)
    # v1: stock bytes + shim at SHIM_OFF; code1 -> shim
    k = bytearray(stock)
    assert SHIM_OFF + len(shim) < len(k)
    k[SHIM_OFF:SHIM_OFF + len(shim)] = shim
    struct.pack_into("<I", k, 4, b_to(4, SHIM_OFF))
    variants["v1_stock_shim"] = lz4_legacy(bytes(k), tmp)
    # v2/v3: shim with stock-like header
    fixed = fix_header(shim)
    variants["v2_shim_fmt"] = fixed
    variants["v3_shim_fmt_lz4"] = lz4_legacy(fixed, tmp)
    # v4: control = the header every earlier hardware run used (flags=0, no MZ)
    k = bytearray(shim); struct.pack_into("<Q", k, 24, 0)
    variants["v4_shim_orig"] = bytes(k)

    avb = stock_avb(open(stock_path, "rb").read())   # ABL needs the AVB footer
    for name, kern in variants.items():
        path = os.path.join(outdir, name + ".img")
        img = repack(hdr, kern, rd, tail)
        open(path, "wb").write(add_avb_footer(img, avb) if avb else img)
        print(f"{path}: kernel {len(kern)} B")


if __name__ == "__main__":
    main()
