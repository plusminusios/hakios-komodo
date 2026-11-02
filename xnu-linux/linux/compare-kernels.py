#!/usr/bin/env python3
"""Compare two Mach-O kernels (macOS-built and Linux-built) without otool.

usage: compare-kernels.py <kernel-a> <kernel-b>

Prints size, Mach-O header fields, the segment table and the symbol summary. Used to answer
"is the Linux kernel the same kind of object as the macOS one, and is it the same size?".
"""
import struct
import sys

MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2
LC_UUID = 0x1B
CPU = {0x0100000C: "arm64", 0x01000007: "x86_64", 0x0000000C: "arm"}
FILETYPE = {2: "MH_EXECUTE", 6: "MH_DYLIB", 1: "MH_OBJECT", 11: "MH_KEXT_BUNDLE"}


def load(path):
    data = open(path, "rb").read()
    magic, cputype, cpusub, ftype, ncmds, sizeofcmds, flags = struct.unpack_from("<IiiIIII", data, 0)
    assert magic == MH_MAGIC_64, hex(magic)
    off = 32
    segments, symtab, uuid = [], None, None
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, off)
        if cmd == LC_SEGMENT_64:
            name = data[off + 8:off + 24].rstrip(b"\0").decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<QQQQ", data, off + 24)
            nsects = struct.unpack_from("<I", data, off + 64)[0]
            segments.append((name, vmaddr, vmsize, fileoff, filesize, nsects))
        elif cmd == LC_SYMTAB:
            symtab = struct.unpack_from("<IIII", data, off + 8)
        elif cmd == LC_UUID:
            uuid = data[off + 8:off + 24].hex()
        off += cmdsize
    return dict(path=path, size=len(data), cputype=cputype, cpusub=cpusub, filetype=ftype,
                ncmds=ncmds, sizeofcmds=sizeofcmds, flags=flags, segments=segments,
                symtab=symtab, uuid=uuid)


def main():
    a, b = load(sys.argv[1]), load(sys.argv[2])
    print("%-28s %22s %22s" % ("", a["path"], b["path"]))
    for key in ("size", "cputype", "filetype", "ncmds", "sizeofcmds", "flags", "uuid"):
        va, vb = a[key], b[key]
        if key == "cputype":
            va, vb = "%s (%#x)" % (CPU.get(va, "?"), va), "%s (%#x)" % (CPU.get(vb, "?"), vb)
        elif key == "filetype":
            va, vb = FILETYPE.get(va, str(va)), FILETYPE.get(vb, str(vb))
        elif key == "size":
            va, vb = "%d (%.2f MiB)" % (va, va / 1048576), "%d (%.2f MiB)" % (vb, vb / 1048576)
        print("%-28s %22s %22s" % (key, va, vb))
    segs = {s[0]: s for s in a["segments"]}
    segs_b = {s[0]: s for s in b["segments"]}
    print("\nsegments (name, vmsize, filesize, sections)")
    for name in sorted(set(segs) | set(segs_b)):
        sa = segs.get(name)
        sb = segs_b.get(name)
        fa = "%d/%d/%d" % (sa[2], sa[4], sa[5]) if sa else "-"
        fb = "%d/%d/%d" % (sb[2], sb[4], sb[5]) if sb else "-"
        print("  %-20s %22s %22s" % (name, fa, fb))
    print("\nsymtab (symoff, nsyms, stroff, strsize):", a["symtab"], b["symtab"])

    # symbol names, so the two kernels can be asked for the same drivers
    names = sys.argv[3:]
    for k in (a, b):
        data = open(k["path"], "rb").read()
        symoff, nsyms, stroff, strsize = k["symtab"]
        strs = data[stroff:stroff + strsize]
        found = 0
        table = set()
        for i in range(nsyms):
            n_strx = struct.unpack_from("<I", data, symoff + i * 16)[0]
            end = strs.find(b"\0", n_strx)
            table.add(strs[n_strx:end].decode("utf-8", "replace"))
        k["symbols"] = table
        print("%s: %d symbols" % (k["path"], nsyms))
    if names:
        print("\n%-32s %6s %6s" % ("symbol", "a", "b"))
        for n in names:
            print("%-32s %6s %6s" % (n, n in a["symbols"], n in b["symbols"]))


if __name__ == "__main__":
    main()
