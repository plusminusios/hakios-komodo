#!/usr/bin/env python3
"""Synthetic Mach-O with LC_DYLD_CHAINED_FIXUPS -> mkpayload_komodo.build().
Checks: __PAGEZERO first (fixup table indexed by ALL segments), a format-6
(64_OFFSET) chain with next links, a page with several chains (START_MULTI),
high8 preserved, untouched data intact, entry from LC_UNIXTHREAD."""
import os, struct, sys, tempfile
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import mkpayload_komodo as mp

TEXT_VA = 0xfffffe0007004000
PG = 0x4000

def ptr64_offset(target_off, nxt, high8=0):
    return (target_off & ((1 << 36) - 1)) | (high8 << 36) | (nxt << 51)

def build_macho():
    # file layout: [0,PG) __TEXT (hdr + code), [PG,3PG) __DATA (2 pages), [3PG,..) __LINKEDIT
    data = bytearray(2 * PG)
    for i in range(0, len(data), 8):
        struct.pack_into("<Q", data, i, 0x5A5A5A5A5A5A5A5A)   # sentinel
    # page 0: one chain 0x10 -> 0x30 -> 0x40 (next in 4-byte units)
    struct.pack_into("<Q", data, 0x10, ptr64_offset(0x1234, (0x30 - 0x10) // 4))
    struct.pack_into("<Q", data, 0x30, ptr64_offset(0x2000, (0x40 - 0x30) // 4, high8=0x80))
    struct.pack_into("<Q", data, 0x40, ptr64_offset(0x4008, 0))
    # page 1: two separate chains (MULTI): at 0x100 and at 0x800
    struct.pack_into("<Q", data, PG + 0x100, ptr64_offset(0x10, 0))
    struct.pack_into("<Q", data, PG + 0x800, ptr64_offset(0x20, 2))
    struct.pack_into("<Q", data, PG + 0x808, ptr64_offset(0x30, 0))

    # chained fixups blob
    nsegs = 4                                     # PAGEZERO, TEXT, DATA, LINKEDIT
    hdr = 28
    starts_img = hdr
    seg_info_off = 4 + 4 * nsegs                  # relative to starts_in_image
    page_count = 2
    # page_start[0]=0x10, page_start[1]=MULTI|2 ; overflow: [2]=0x100, [3]=0x800|LAST
    starts = [0x10, mp.CHAINED_START_MULTI | 2, 0x100, 0x800 | mp.CHAINED_START_LAST]
    sis = struct.pack("<IHHQIH", 22 + 2 * len(starts), PG, 6, PG, 0, page_count)
    sis += struct.pack("<%dH" % len(starts), *starts)
    img = struct.pack("<I", nsegs) + struct.pack("<4I", 0, 0, seg_info_off, 0)
    blob = struct.pack("<7I", 0, starts_img, 0, 0, 0, 1, 0) + img + sis
    blob += b"\0" * (-len(blob) & 7)

    linkedit_off = 3 * PG
    # load commands
    def seg(name, va, vs, fo, fs):
        return struct.pack("<II16sQQQQIIII", 0x19, 72, name.encode(), va, vs, fo, fs, 7, 5, 0, 0)
    entry_va = TEXT_VA + 0x800
    th = struct.pack("<IIII", 0x5, 16 + 4 * 2 + 0, 6, 68)   # placeholder size fixed below
    state = [0] * 34; state[32] = entry_va
    th = struct.pack("<IIII", 0x5, 16 + 34 * 8, 6, 68) + struct.pack("<34Q", *state)
    lcs = seg("__PAGEZERO", 0, 0x100000000, 0, 0)
    lcs += seg("__TEXT", TEXT_VA, PG, 0, PG)
    lcs += seg("__DATA", TEXT_VA + PG, 2 * PG, PG, 2 * PG)
    lcs += seg("__LINKEDIT", TEXT_VA + 3 * PG, PG, linkedit_off, len(blob))
    lcs += th
    lcs += struct.pack("<IIII", mp.LC_DYLD_CHAINED_FIXUPS, 16, linkedit_off, len(blob))
    mh = struct.pack("<8I", 0xfeedfacf, 0x0100000c, 0, 2, 6, len(lcs), 0, 0)
    text = bytearray(PG); text[:len(mh) + len(lcs)] = mh + lcs
    return bytes(text) + bytes(data) + blob, entry_va

def main():
    macho, entry_va = build_macho()
    if "--write-macho" in sys.argv:          # for end-to-end `make bootimg` tests
        open(sys.argv[sys.argv.index("--write-macho") + 1], "wb").write(macho)
        return
    d = tempfile.mkdtemp()
    k = os.path.join(d, "k"); a = os.path.join(d, "a")
    open(k, "wb").write(macho); open(a, "wb").write(b"\xAD" * 64)
    out = mp.build(k, a, 0x98000000, 0x2b000000, "-v debug=0x8")
    nsegs, = struct.unpack_from("<I", out, 12)
    ent, = struct.unpack_from("<Q", out, 0x18)
    recs = [struct.unpack_from("<QQQQ", out, mp.SEGS_OFF + 32 * i) for i in range(nsegs)]
    data_rec = [r for r in recs if r[0] == TEXT_VA + PG][0]
    base = data_rec[2]
    q = lambda off: struct.unpack_from("<Q", out, base + off)[0]
    ok = True
    def chk(name, c):
        nonlocal ok; ok &= bool(c); print(f"  {name:48s} {'ok' if c else 'FAIL'}")
    chk("PAGEZERO not loaded (3 segs)", nsegs == 3)
    chk("entry from LC_UNIXTHREAD", ent == entry_va)
    chk("page0 chain[0] -> image_base+0x1234", q(0x10) == TEXT_VA + 0x1234)
    chk("page0 chain[1] high8 kept", q(0x30) == (TEXT_VA + 0x2000) | (0x80 << 56))
    chk("page0 chain[2] (end)", q(0x40) == TEXT_VA + 0x4008)
    chk("page1 MULTI chain A", q(PG + 0x100) == TEXT_VA + 0x10)
    chk("page1 MULTI chain B[0]", q(PG + 0x800) == TEXT_VA + 0x20)
    chk("page1 MULTI chain B[1]", q(PG + 0x808) == TEXT_VA + 0x30)
    chk("untouched data intact", q(0x18) == 0x5A5A5A5A5A5A5A5A and q(PG + 0x108) == 0x5A5A5A5A5A5A5A5A)
    chk("cmdline packed", out[0x48:0x48 + 12] == b"-v debug=0x8")
    # --ramdisk: extra segment after the kernel, ADT gets /chosen/memory-map RAMDisk
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    import mkadt_komodo as adt
    adtp = os.path.join(d, "real.adt"); open(adtp, "wb").write(adt.serialize(adt.build()))
    rdp = os.path.join(d, "rd"); rd = bytes(range(256)) * 300 + b"END"
    open(rdp, "wb").write(rd)
    out2 = mp.build(k, adtp, 0x98000000, 0x2b000000, "rd=md0", rdp)
    n2, = struct.unpack_from("<I", out2, 12)
    recs2 = [struct.unpack_from("<QQQQ", out2, mp.SEGS_OFF + 32 * i) for i in range(n2)]
    va, vs, fo, fs = recs2[-1]
    kend, = struct.unpack_from("<Q", out2, 0x30)
    adt_off, adt_size = struct.unpack_from("<QQ", out2, 0x20)
    chk("ramdisk = extra segment", n2 == nsegs + 1 and fs == len(rd) and out2[fo:fo + fs] == rd)
    chk("ramdisk 16K aligned, kernel_end past it", va % 0x4000 == 0 and kend == va + vs)
    root, _ = adt._parse(out2[adt_off:adt_off + adt_size])
    chosen = next(c for c in root.children if adt._name(c) == "chosen")
    mm = next(c for c in chosen.children if adt._name(c) == "memory-map")
    rdp_ = [v for kk, _, v in mm.props if kk == "RAMDisk"]
    vbase, = struct.unpack_from("<Q", out2, 0x10)
    chk("virt_base = header's 32 MiB block", vbase == TEXT_VA & ~(0x2000000 - 1))
    exp_pa = 0x98000000 + (va - vbase)
    chk("ADT memory-map RAMDisk = <phys,size16K>", rdp_ and struct.unpack("<2Q", rdp_[0]) == (exp_pa, (len(rd) + 0x3fff) & ~0x3fff))
    chk("fb placeholder survives ADT edit", struct.pack("<Q", 0x4b4f4d4f46424144) in out2[adt_off:adt_off + adt_size])
    chk("segment table not overlapping data", recs2[0][2] >= mp.SEGS_OFF + 32 * n2)
    print("RESULT:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)

main()
