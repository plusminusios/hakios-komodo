#!/usr/bin/env python3
"""Unpack Android boot image v4 / vendor_boot v4 / dtbo images and dump every
FDT (DTB) blob they contain, for hardware research on the Pixel 9a (tegu).

Usage: tools/unpack_bootv4.py boot.img vendor_boot.img dtbo.img OUTDIR

The DTB blobs are the shim's ground truth: the shim parses the *live* FDT the
bootloader passes, and these dumps tell us which nodes exist (memory, chosen,
framebuffer, touch, PMIC...). Feed confirmed findings back into NOTES.md.
"""
import os
import struct
import sys

FDT_MAGIC = 0xD00DFEED


def u32(buf, off):
    return struct.unpack_from(">I", buf, off)[0]


def scan_fdts(data, outdir, tag):
    """Scan for FDT magic and dump every well-formed blob found."""
    found = 0
    off = 0
    while True:
        off = data.find(b"\xd0\x0d\xfe\xed", off)
        if off < 0:
            break
        totalsize = u32(data, off + 4)
        if 0 < totalsize <= len(data) - off:
            name = os.path.join(outdir, f"{tag}-{off:08x}.dtb")
            with open(name, "wb") as f:
                f.write(data[off:off + totalsize])
            print(f"  [dtb] {name} ({totalsize} bytes) at offset {off:#x}")
            found += 1
            off += totalsize
        else:
            off += 4
    return found


def info_boot_v4(data, outdir):
    magic = data[:8]
    if magic != b"ANDROID!":
        print(f"  not an Android boot image: {magic!r}")
        return
    kernel_size = struct.unpack_from("<I", data, 8)[0]
    header_version = struct.unpack_from("<I", data, 40)[0]
    page_size = struct.unpack_from("<I", data, 36)[0]
    print(f"  boot img v{header_version}, page {page_size}, kernel {kernel_size} bytes")
    # v4: kernel starts at page 1; ramdisk lives in vendor_boot, not here.
    with open(os.path.join(outdir, "kernel.raw.gz"), "wb") as f:
        f.write(data[page_size:page_size + kernel_size])
    n = scan_fdts(data, outdir, "boot")
    if n == 0:
        print("  no DTB in boot.img (expected: it lives in vendor_boot/dtbo on GKI)")


def info_vendor_boot(data, outdir):
    if data[:8] != b"VNDRBOOT":
        print(f"  not a vendor_boot image: {data[:8]!r}")
        return
    header_size = struct.unpack_from("<I", data, 8)[0]
    page_size = struct.unpack_from("<I", data, 12)[0]
    dtb_size = struct.unpack_from("<I", data, 48)[0]
    print(f"  vendor_boot header {header_size} bytes, page {page_size}, dtb {dtb_size} bytes")
    n = scan_fdts(data, outdir, "vendorboot")
    if n == 0 and dtb_size:
        print(f"  header says dtb exists ({dtb_size} bytes) but no FDT magic found: dump manually")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    outdir = sys.argv[-1]
    os.makedirs(outdir, exist_ok=True)
    for path in sys.argv[1:-1]:
        print(f"== {path}")
        with open(path, "rb") as f:
            data = f.read()
        if data[:8] == b"VNDRBOOT":
            info_vendor_boot(data, outdir)
        else:
            info_boot_v4(data, outdir)
    print("\nNext: dtc -I dtb -O dts <dump> to decompile and read the nodes.")


if __name__ == "__main__":
    main()
