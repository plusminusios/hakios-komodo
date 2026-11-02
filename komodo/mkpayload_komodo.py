#!/usr/bin/env python3
"""Pack an XNU kernel Mach-O + ADT into the "KOMOXNU" payload that the komodo
shim loader (shim/loader.c) boots.

The payload is a flat header + segment table, then each segment's file bytes,
then the ADT. The shim copies each segment to phys_base + (vmaddr - virt_base),
places the ADT + boot_args behind the kernel, and jumps to the entry.

Header layout MUST match `struct payload` in shim/loader.c:
  off 0x000  char magic[8]       "KOMOXNU\\0"
  off 0x008  u32  version        (1)
  off 0x00c  u32  nsegs
  off 0x010  u64  virt_base
  off 0x018  u64  entry_va
  off 0x020  u64  adt_off
  off 0x028  u64  adt_size
  off 0x030  u64  kernel_end
  off 0x038  u64  phys_base
  off 0x040  u64  mem_size
  off 0x048  char cmdline[608]
  off 0x2a8  struct { u64 vmaddr, vmsize, file_off, file_size } segs[nsegs]
then segment data (each at its file_off), then ADT (at adt_off).

The kernel is linked as a PIE with LC_DYLD_CHAINED_FIXUPS. On Apple hardware
iBoot/kmutil resolve those; here nobody does, and xnu only rebases static
kernelcaches itself. So every chained rebase is rewritten to its plain
(unslid) target VA before the segments are packed, and the shim only copies
flat bytes. Binds are refused (the kernel has none).

Usage:
  mkpayload_komodo.py KERNEL.macho ADT.bin OUT.bin \\
        [--phys 0x98000000] [--mem 0x2b000000] [--cmdline "-v debug=0x8"]
  mkpayload_komodo.py --selftest
"""
import argparse, struct, sys

MAGIC = b"KOMOXNU\0"
BOOT_LINE_LENGTH = 608
HDR_FIXED = 0x48                      # bytes before cmdline
SEGS_OFF  = 0x2a8                     # struct payload: offsetof(segs)
PHYS_DEFAULT = 0x98000000             # largest reserved-memory hole <4G (see shim/Makefile)
MEM_DEFAULT  = 0x2b000000             # up to 0xc3000000 (seclog_mem)

# Mach-O
MH_MAGIC_64 = 0xfeedfacf
LC_SEGMENT_64 = 0x19
LC_UNIXTHREAD = 0x5
LC_MAIN       = 0x80000028
LC_DYLD_CHAINED_FIXUPS = 0x80000034
CHAINED_START_NONE  = 0xffff
CHAINED_START_MULTI = 0x8000
CHAINED_START_LAST  = 0x8000

def pack_header(nsegs, virt_base, entry_va, adt_off, adt_size, kernel_end,
                phys_base, mem_size, cmdline):
    cl = cmdline.encode()[:BOOT_LINE_LENGTH - 1]
    cl = cl + b"\0" * (BOOT_LINE_LENGTH - len(cl))
    h = MAGIC
    h += struct.pack("<II", 1, nsegs)
    h += struct.pack("<QQQQQQQ", virt_base, entry_va, adt_off, adt_size,
                     kernel_end, phys_base, mem_size)
    assert len(h) == HDR_FIXED, (len(h), HDR_FIXED)
    h += cl
    assert len(h) == SEGS_OFF, (len(h), SEGS_OFF)
    return h

def parse_macho(buf):
    """Return dict(segs_all, segs, virt_base, entry, kernel_end, image_base, fixups).
    segs_all keeps EVERY LC_SEGMENT_64 in load-command order: the chained
    fixups table is indexed by that order. segs = the ones that get loaded."""
    magic, cputype, cpusub, filetype, ncmds, sizeofcmds, flags, _ = \
        struct.unpack_from("<IIIIIIII", buf, 0)
    if magic != MH_MAGIC_64:
        raise SystemExit("not a 64-bit Mach-O (magic %#x)" % magic)
    segs_all = []      # (name, vmaddr, vmsize, fileoff, filesize)
    entry = None
    fixups = None
    off = 32
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", buf, off)
        if cmd == LC_SEGMENT_64:
            name = buf[off+8:off+24].split(b"\0")[0].decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<QQQQ", buf, off+24)
            segs_all.append((name, vmaddr, vmsize, fileoff, filesize))
        elif cmd == LC_UNIXTHREAD:
            # ARM_THREAD_STATE64: flavor,count then x0..x28, fp, lr, sp, pc, cpsr
            entry = struct.unpack_from("<Q", buf, off + 16 + 32*8)[0]
        elif cmd == LC_MAIN:
            entryoff, = struct.unpack_from("<Q", buf, off+8)
            entry = ("main", entryoff)
        elif cmd == LC_DYLD_CHAINED_FIXUPS:
            fixups = struct.unpack_from("<II", buf, off+8)    # dataoff, datasize
        off += cmdsize
    segs = [(v, vs, fo, fs) for (n, v, vs, fo, fs) in segs_all
            if n != "__PAGEZERO" and vs > 0]
    if not segs:
        raise SystemExit("no loadable segments")
    kernel_end = max(x[0] + x[1] for x in segs)
    # image base for *_OFFSET pointer formats: the segment holding the header
    hdr_segs = [v for (n, v, vs, fo, fs) in segs_all if fo == 0 and fs > 0]
    image_base = hdr_segs[0] if hdr_segs else min(x[0] for x in segs)
    # virt_base (the VA that maps to phys_base): the 32 MiB block (16K-granule
    # L2) holding the mach header. XNU's start.s maps the kernel in 32 MiB
    # blocks from virtBase -> physBase; an unaligned virtBase would shift that
    # mapping and fault right after the MMU comes on. (Same rule as ios-tegu.)
    virt_base = image_base & ~(0x2000000 - 1)
    if min(x[0] for x in segs) < virt_base:
        raise SystemExit("a segment lies below the header's 32 MiB block")
    if isinstance(entry, tuple):         # LC_MAIN: fileoff -> vmaddr
        entry = image_base + entry[1]
    if entry is None:
        entry = virt_base
    return dict(segs_all=segs_all, segs=segs, virt_base=virt_base, entry=entry,
                kernel_end=kernel_end, image_base=image_base, fixups=fixups)


def chained_decode(raw, fmt, image_base):
    """(target VA, next, stride) for one chained REBASE pointer."""
    if fmt in (2, 6):        # DYLD_CHAINED_PTR_64 / DYLD_CHAINED_PTR_64_OFFSET
        if raw >> 63:
            raise SystemExit("chained fixups: kernel has a bind (unsupported)")
        target = raw & ((1 << 36) - 1)
        high8 = (raw >> 36) & 0xff
        nxt = (raw >> 51) & 0xfff
        if fmt == 6:
            target += image_base
        return target | (high8 << 56), nxt, 4
    if fmt == 8:             # DYLD_CHAINED_PTR_64_KERNEL_CACHE
        if raw >> 63:
            raise SystemExit("chained fixups: authenticated pointer (arm64e) in an arm64 kernel")
        target = raw & ((1 << 30) - 1)
        nxt = (raw >> 51) & 0xfff
        return image_base + target, nxt, 4
    raise SystemExit("chained fixups: unsupported pointer format %d" % fmt)


def apply_chained_fixups(buf, m):
    """Rewrite every chained rebase in buf (bytearray, whole file) in place.
    Returns the number of pointers fixed."""
    if not m["fixups"]:
        return 0
    data_off, _ = m["fixups"]
    (_ver, starts_off, _imp_off, _sym_off, imports_count,
     _imp_fmt, _sym_fmt) = struct.unpack_from("<7I", buf, data_off)
    if imports_count:
        raise SystemExit("chained fixups: %d imports (binds) - not a static kernel" % imports_count)
    sbase = data_off + starts_off
    seg_count, = struct.unpack_from("<I", buf, sbase)
    if seg_count > len(m["segs_all"]):
        raise SystemExit("chained fixups: %d segment entries but only %d segments"
                         % (seg_count, len(m["segs_all"])))
    nfix = 0
    for i in range(seg_count):
        seg_info, = struct.unpack_from("<I", buf, sbase + 4 + 4*i)
        if not seg_info:
            continue
        b = sbase + seg_info
        _size, page_size, fmt, _seg_off, _maxv, page_count = \
            struct.unpack_from("<IHHQIH", buf, b)
        starts = struct.unpack_from("<%dH" % page_count, buf, b + 22)
        fileoff = m["segs_all"][i][3]

        def walk(loc):
            n = 0
            while True:
                raw, = struct.unpack_from("<Q", buf, loc)
                target, nxt, stride = chained_decode(raw, fmt, m["image_base"])
                struct.pack_into("<Q", buf, loc, target)
                n += 1
                if not nxt:
                    return n
                loc += nxt * stride

        for pg, start in enumerate(starts):
            if start == CHAINED_START_NONE:
                continue
            page = fileoff + pg * page_size
            if start & CHAINED_START_MULTI:
                # overflow list after page_start[page_count]: several chains on
                # this page, the last one flagged with CHAINED_START_LAST
                idx = start & ~CHAINED_START_MULTI
                while True:
                    s2, = struct.unpack_from("<H", buf, b + 22 + 2*idx)
                    nfix += walk(page + (s2 & ~CHAINED_START_LAST))
                    if s2 & CHAINED_START_LAST:
                        break
                    idx += 1
            else:
                nfix += walk(page + start)
    return nfix

PAGE16K = 0x4000

def adt_add_ramdisk(adt_blob, phys, size):
    """ADT with /chosen/memory-map RAMDisk = <phys, size> (two u64). XNU's
    IOFindBSDRoot turns it into md0 (boot with rd=md0)."""
    import os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import mkadt_komodo as adt
    root, _ = adt._parse(adt_blob)
    chosen = next(c for c in root.children if adt._name(c) == "chosen")
    mmap = next(c for c in chosen.children if adt._name(c) == "memory-map")
    if any(k == "RAMDisk" for k, _, _ in mmap.props):
        raise SystemExit("ADT already has a RAMDisk")
    mmap.props.append(("RAMDisk", 0, struct.pack("<2Q", phys, size)))
    return adt.serialize(root)


def build(kernel_path, adt_path, phys_base, mem_size, cmdline, ramdisk_path=None):
    kbuf = bytearray(open(kernel_path, "rb").read())
    adt  = open(adt_path, "rb").read()
    m = parse_macho(kbuf)
    nfix = apply_chained_fixups(kbuf, m)
    print(f"chained fixups: {nfix} rebases resolved", file=sys.stderr)
    segs, virt_base, entry_va, kernel_end = m["segs"], m["virt_base"], m["entry"], m["kernel_end"]
    rd_seg = None
    if ramdisk_path:
        # one more flat segment right after the kernel: the shim copies it like
        # any other, it stays below topOfKernelData (never handed to the VM)
        rd = open(ramdisk_path, "rb").read()
        rd_va = (kernel_end + PAGE16K - 1) & ~(PAGE16K - 1)
        rd_vs = (len(rd) + PAGE16K - 1) & ~(PAGE16K - 1)
        rd_pa = phys_base + (rd_va - virt_base)
        kernel_end = rd_va + rd_vs
        # size rounded UP to the 16K page (as ios-tegu): md0 truncates to whole
        # blocks, and a static init's code signature sits right past 0x4000 --
        # an exact, unaligned size cut it off (komodo: "load code signature
        # error 2" = LOAD_BADMACHO). The shim zero-fills the segment tail.
        adt = adt_add_ramdisk(adt, rd_pa, rd_vs)
        rd_seg = (rd_va, rd_vs, rd)
        print(f"ramdisk: {len(rd)} bytes at PA {rd_pa:#x} (boot with rd=md0)", file=sys.stderr)
        if kernel_end - virt_base + len(adt) + 2 * PAGE16K > mem_size:
            raise SystemExit("kernel + ramdisk do not fit the RAM window (--mem)")

    # Lay out: header | seg data (8-byte aligned) | adt (8-byte aligned)
    body = bytearray()
    seg_records = []
    cur = SEGS_OFF + (len(segs) + (1 if rd_seg else 0)) * 32
    cur = (cur + 7) & ~7
    # We place file bytes sequentially; file_off is the payload offset.
    data = bytearray()
    for (vmaddr, vmsize, fileoff, filesize) in segs:
        foff = cur + len(data)
        data += kbuf[fileoff:fileoff+filesize]
        if len(data) & 7:
            data += b"\0" * (-len(data) & 7)
        seg_records.append((vmaddr, vmsize, foff, filesize))
    if rd_seg:
        rd_va, rd_vs, rd = rd_seg
        foff = cur + len(data)
        data += rd
        if len(data) & 7:
            data += b"\0" * (-len(data) & 7)
        seg_records.append((rd_va, rd_vs, foff, len(rd)))
    adt_off = cur + len(data)
    data += adt
    if len(data) & 7:
        data += b"\0" * (-len(data) & 7)

    hdr = pack_header(len(seg_records), virt_base, entry_va, adt_off, len(adt),
                      kernel_end, phys_base, mem_size, cmdline)
    segtab = b"".join(struct.pack("<QQQQ", *r) for r in seg_records)
    pad = b"\0" * (cur - (SEGS_OFF + len(segtab)))
    return bytes(hdr) + segtab + pad + bytes(data)

def selftest():
    # Verify the header layout matches loader.c's struct exactly.
    h = pack_header(2, 0xfffffe0007004000, 0xfffffe0007005000, 0x3000, 0x40,
                    0xfffffe000700c000, 0x80000000, 0x40000000, "-v komodo")
    assert h[0:8] == MAGIC
    assert struct.unpack_from("<I", h, 8)[0] == 1          # version
    assert struct.unpack_from("<I", h, 12)[0] == 2         # nsegs
    assert struct.unpack_from("<Q", h, 16)[0] == 0xfffffe0007004000
    assert struct.unpack_from("<Q", h, 0x38)[0] == 0x80000000   # phys_base
    assert struct.unpack_from("<Q", h, 0x40)[0] == 0x40000000   # mem_size
    assert h[0x48:0x48+10] == b"-v komodo\0"
    assert len(h) == SEGS_OFF == 0x2a8
    print("selftest: header layout matches loader.c (segs @ 0x2a8) -- OK")

if __name__ == "__main__":
    if "--selftest" in sys.argv:
        selftest(); sys.exit(0)
    ap = argparse.ArgumentParser(description="pack XNU + ADT into a KOMOXNU payload")
    ap.add_argument("kernel"); ap.add_argument("adt"); ap.add_argument("out")
    ap.add_argument("--phys", type=lambda x: int(x, 0), default=PHYS_DEFAULT)
    ap.add_argument("--mem", type=lambda x: int(x, 0), default=MEM_DEFAULT)
    ap.add_argument("--cmdline", default="-v")
    ap.add_argument("--ramdisk", help="image for md0 (e.g. a static init Mach-O for mockfs)")
    a = ap.parse_args()
    out = build(a.kernel, a.adt, a.phys, a.mem, a.cmdline, a.ramdisk)
    open(a.out, "wb").write(out)
    print(f"{a.out}: {len(out)} bytes", file=sys.stderr)
