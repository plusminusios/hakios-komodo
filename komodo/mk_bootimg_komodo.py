#!/usr/bin/env python3
"""Minimal Android boot image v4 builder for the komodo shim.

Lays out: pad(header) + pad(kernel) + pad(ramdisk).  Page size is fixed 4096
for header v3/v4.  The shim Image is the kernel; the KOMOXNU payload (XNU +
ADT, from mkpayload_komodo.py) is the ramdisk — ABL loads it and passes its
address in /chosen linux,initrd-start/-end, which loader.c reads.

For a diagnostic boot (just prove the shim runs), pass no ramdisk: the shim
still comes up and prints FDT/MEM/GICD, then reports "no KOMOXNU payload".

  mk_bootimg_komodo.py KERNEL OUT [--ramdisk RD] [--cmdline STR]
                       [--os_version X.Y.Z] [--os_patch YYYY-MM]
"""
import sys, struct

PAGE = 4096
HEADER_VERSION = 4
HEADER_SIZE = 1584            # boot_img_hdr_v4
BOOT_ARGS = 512
BOOT_EXTRA = 1024

def pad(data, page=PAGE):
    r = len(data) % page
    return data + (b'\x00' * (page - r) if r else b'')

def enc_os_version(ver, patch):
    """AOSP os_version|os_patch_level packing: 11+11+10 bits for A.B.C, then
    7+4 bits for year-2000 and month."""
    a = b = c = 0
    if ver:
        parts = [int(x) for x in ver.split('.')] + [0, 0]
        a, b, c = parts[0], parts[1], parts[2]
    y = m = 0
    if patch:
        yy, mm = patch.split('-')
        y, m = int(yy) - 2000, int(mm)
    # AOSP: os_version = 7+7+7 bits (A.B.C) = 21 bits; os_patch = 7+4 bits = 11.
    version = ((a & 0x7f) << 14) | ((b & 0x7f) << 7) | (c & 0x7f)
    patchl = ((y & 0x7f) << 4) | (m & 0xf)
    return ((version << 11) | patchl) & 0xffffffff

def main():
    a = sys.argv
    if len(a) < 3:
        print(__doc__); sys.exit(2)
    kernel_path, out_path = a[1], a[2]
    rd_path  = a[a.index('--ramdisk')+1]  if '--ramdisk'  in a else None
    cmdline  = a[a.index('--cmdline')+1]  if '--cmdline'  in a else ""
    osver    = a[a.index('--os_version')+1] if '--os_version' in a else ""
    ospatch  = a[a.index('--os_patch')+1]   if '--os_patch'   in a else ""

    kernel = open(kernel_path, 'rb').read()
    ramdisk = open(rd_path, 'rb').read() if rd_path else b''
    cl = cmdline.encode()
    assert len(cl) <= BOOT_ARGS + BOOT_EXTRA, "cmdline too long"

    hdr  = b'ANDROID!'
    hdr += struct.pack('<I', len(kernel))
    hdr += struct.pack('<I', len(ramdisk))
    hdr += struct.pack('<I', enc_os_version(osver, ospatch))
    hdr += struct.pack('<I', HEADER_SIZE)
    hdr += struct.pack('<IIII', 0, 0, 0, 0)        # reserved[4]
    hdr += struct.pack('<I', HEADER_VERSION)
    hdr += cl + b'\x00' * (BOOT_ARGS + BOOT_EXTRA - len(cl))
    hdr += struct.pack('<I', 0)                    # signature_size (v4)
    assert len(hdr) == HEADER_SIZE, (len(hdr), HEADER_SIZE)

    img = pad(hdr) + pad(kernel)
    if ramdisk:
        img += pad(ramdisk)
    open(out_path, 'wb').write(img)
    print(f"wrote {out_path}: {len(img)} bytes "
          f"(kernel {len(kernel)}, ramdisk {len(ramdisk)}, v{HEADER_VERSION})")

if __name__ == '__main__':
    main()
