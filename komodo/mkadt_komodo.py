#!/usr/bin/env python3
"""Build the minimal Apple Device Tree (ADT) that XNU needs to boot on the
Pixel 9 Pro XL (komodo / Tensor G4 / zumapro).

Self-contained: its own ADT serializer (no external import), so it can be run
and verified offline.  Design follows dipa-studio/ios-sweet -> LeR33/ios-tegu
(the Pixel 9a port on the SAME SoC, zumapro); every komodo MMIO value below is
verified against the live device (/proc/iomem, /proc/device-tree, dumpsys).

Deltas from tegu (same SoC): panel 1344x2992 (tegu 1080x2406), 16 GiB RAM,
display dpi ~487.  GIC / UFS / timer / USB / ramoops / UART are identical.

Usage:  mkadt_komodo.py OUT.adt [--dump] [--qemu] [--highmem]

ADT layout (little-endian):
  node = {u32 nprops, u32 nchildren, props..., children...}
  prop = {char name[32], u32 size (bit31 = placeholder flag), value padded to 4}
"""
import hashlib, os, struct, sys

# ---------------------------------------------------------------------------
# Self-contained ADT serializer (format per Apple device tree / ios-sweet).
# ---------------------------------------------------------------------------

class Node:
    def __init__(self):
        self.props = []      # list of (name:str, flags:int, value:bytes)
        self.children = []   # list of Node

def serialize(node):
    out = bytearray(struct.pack('<II', len(node.props), len(node.children)))
    for name, flags, val in node.props:
        out += name.encode().ljust(32, b'\0')[:32]
        out += struct.pack('<I', len(val) | (flags << 31))
        out += val + b'\0' * (-len(val) & 3)
    for c in node.children:
        out += serialize(c)
    return bytes(out)

def _parse(buf, off=0):
    n = Node()
    nprops, nch = struct.unpack_from('<II', buf, off); off += 8
    for _ in range(nprops):
        name = buf[off:off+32].split(b'\0')[0].decode()
        size, = struct.unpack_from('<I', buf, off+32)
        flags, ln = size >> 31, size & 0x7fffffff
        n.props.append((name, flags, buf[off+36:off+36+ln]))
        off += 36 + ((ln + 3) & ~3)
    for _ in range(nch):
        c, off = _parse(buf, off); n.children.append(c)
    return n, off

def _name(n):
    for k, _, v in n.props:
        if k == 'name':
            return v.split(b'\0')[0].decode()
    return '?'

def _fmt(v):
    if v and v[-1:] == b'\0' and all(32 <= c < 127 for c in v[:-1].replace(b'\0', b' ')) and len(v) > 1:
        return repr(v.rstrip(b'\0').replace(b'\0', b'|').decode())
    if len(v) in (4, 8):
        return hex(int.from_bytes(v, 'little'))
    if len(v) % 8 == 0 and len(v) <= 64:
        return '<' + ' '.join(hex(x) for x in struct.unpack('<%dQ' % (len(v)//8), v)) + '>'
    if len(v) % 4 == 0 and len(v) <= 64:
        return '<' + ' '.join(hex(x) for x in struct.unpack('<%dI' % (len(v)//4), v)) + '>'
    return v[:48].hex() + ('...' if len(v) > 48 else '') + f' ({len(v)} bytes)'

def dump(node, depth=0):
    ind = '  ' * depth
    print(f'{ind}{_name(node)} {{')
    for k, flags, v in node.props:
        if k == 'name':
            continue
        print(f'{ind}  {k}{" [ph]" if flags else ""} = {_fmt(v)}')
    for c in node.children:
        dump(c, depth + 1)
    print(f'{ind}}}')

# ---------------------------------------------------------------------------
# komodo hardware values (VERIFIED on the live Pixel 9 Pro XL, 2026-11-01)
# ---------------------------------------------------------------------------
PHYS_BASE   = 0x80000000       # DRAM base (/memory@80000000)
MEM_SIZE    = 0x80000000       # low bank 2 GiB window; real 16 GiB filled by loader
# upper DDR bank, VERIFIED from live /proc/iomem 2026-11-02:
# 880000000-bffffffff : System RAM (14 GiB, no reserved holes)
HIGH_PHYS_BASE = 0x880000000
HIGH_MEM_SIZE  = 0x380000000

SOC_BASE = 0x10400000          # = GICD; /arm-io children are relative to this

GICD, GICD_SIZE = 0x10400000, 0x10000     # live /proc/iomem (MATCH tegu)
GICR, GICR_SIZE = 0x10440000, 0x200000    # live /proc/iomem (MATCH tegu)

# Bootloader framebuffer: komodo /reserved-memory/vframe is a shared-dma-pool
# (CMA, 512 MiB) -> RUNTIME address. The device ADT carries FB_PLACEHOLDER in
# /tegu-fbcon reg[0]; the shim loader (loader.c adt_patch_fb) replaces it with
# the address it reads from DPP0 RDMA_BASEADDR at boot. --qemu keeps FB.
FB, FB_SIZE = 0xf0000000, 0x1800000
FB_PLACEHOLDER = 0x4b4f4d4f46424144   # 'KOMOFBAD' as a u64 (must match loader.c)
# Native panel (dumpsys: StableDisplaySize / HWC mode id=0): 1344x2992, ~487 dpi.
FB_W, FB_H, FB_STRIDE = 1344, 2992, 1344 * 4

KOMODO_ECID = 0x4b4f4d4f00000001   # 'KOMO' + 1, synthetic (not the real ECID)

UFS, UFS_SIZE = 0x13200000, 0x200  # samsung,exynos-ufs@13200000 (MATCH tegu)
TIMEBASE = 24576000                # arch timer 24.576 MHz (MATCH tegu)
# Architected VIRTUAL timer (CNTV, what XNU's rtclock uses) = PPI 11 -> INTID 27
# on standard GICv3 (same as QEMU virt; see xnu patch 0017). The tegu project's
# mkadt used 30 (that is the ns-PHYSICAL timer, PPI 14) with a self-contradictory
# comment. We use 27; if the timer never fires on first boot, try 30 (physical).
VTIMER_INTID = 27                  # virtual timer PPI 11 -> INTID 27 (verify; alt 30)

# QEMU virt (-M virt,gic-version=3): for offline smoke only.
QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE = 0x08010000, 0x080a0000, 62500000
QEMU_VTIMER_INTID = 27

BOOT_MANIFEST_HASH = hashlib.sha384(b'komodo boot manifest').digest()

# The iOS userland is the iPhone X (iPhone10,3 / D22AP), as in ios-sweet/tegu.
# /product comes from the real D22AP DeviceTree extracted from the IPSW
# (Apple artifact, kept out of git). Absent -> no /product node (MobileGestalt
# falls back; fine for early bring-up).
D22AP_ADT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'research/d22ap.adt')

u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'

def node(name, props=(), children=()):
    n = Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n

def d22ap_product(path=D22AP_ADT):
    if not os.path.exists(path):
        print(f'warning: {path} missing, no /product node (ok for bring-up)', file=sys.stderr)
        return None
    root, _ = _parse(open(path, 'rb').read())
    for c in root.children:
        if _name(c) == 'product':
            return [(k, v) for k, f, v in c.props if not f and k not in ('name', 'AAPL,phandle')]
    return None

def build(qemu=False, highmem=False, seed=None):
    phys_base, mem_size = (HIGH_PHYS_BASE, HIGH_MEM_SIZE) if highmem else (PHYS_BASE, MEM_SIZE)
    gicd, gicr, timebase = (QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE) if qemu else (GICD, GICR, TIMEBASE)
    soc_base = gicd  # /arm-io ranges are relative to the soc base = GICD in both modes
    seed = seed if seed is not None else os.urandom(64)
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('firmware-version', cstr('komodo-shim')),
        ('system-firmware-version', cstr('komodo-shim')),
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        ('crypto-hash-method', cstr('sha2-384')),
        # iPhone X class UI is @3x.
        ('display-scale', u32(3)),
        ('unique-chip-id', struct.pack('<Q', KOMODO_ECID)),
        ('chip-id', u32(0x8015)),   # A11 chip-id the iPhone X userland expects
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ], [node('memory-map', [ph()]),
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])])

    # komodo CPUs: 8 online (DT has 9 cpu nodes 0x0..0x800, cpu@800 not onlined).
    # 4x Cortex-A520 + 3x Cortex-A720 + 1x Cortex-X4. For bring-up we present a
    # homogeneous cluster in physical-core (MPIDR) order; cpus=N selects a prefix.
    cpu_nodes = []
    for core in range(1 if qemu else 8):
        cpu_nodes.append(node(f'cpu{core}', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', cstr('qcom,kryo470') + cstr('ARM,v8')),
            ('reg', u32(core << 8)),      # MPIDR Aff1:Aff0 (0x0,0x100,...,0x700)
            ('cpu-id', u32(core)),
            ('cluster-id', u32(0)),
            ('cluster-type', u32(0)),
            ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(core)),
            ('l2-cache-id', u32(core)),
            ('l3-cache-id', u32(0)),
            ('state', cstr('running' if core == 0 else 'stopped')),
            ('timebase-frequency', u32(timebase)),
            ('clock-frequency', u32(2800000000 if core >= 7 else (2600000000 if core >= 4 else 1900000000))),
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(1866000000)),
            ('peripheral-frequency', u32(timebase)),
            ('fixed-frequency', u32(timebase)),
        ]))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)), ('#size-cells', u32(0))], cpu_nodes)

    gic = node('gic', [
        ph(),
        ('compatible', cstr('arm,gic-v3')),
        ('interrupt-controller', b''),
        ('virtual-timer-intid', u32(QEMU_VTIMER_INTID if qemu else VTIMER_INTID)),
        ('reg', u64(gicd - soc_base, GICD_SIZE, gicr - soc_base, GICR_SIZE)),
    ])
    arm_io = node('arm-io', [
        ph(),
        # komodo reuses the TEGU board slot: keep tegu's names, the kernel's
        # IOKit personalities (iokit/KernelConfigTables.cpp) match on them.
        ('device_type', cstr('tegu-io')),
        ('compatible', cstr('tegu-io') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, soc_base, 0x100000000 - soc_base)),
    ], [gic,
        node('wdt', [ph(), ('no-pmu', u32(1))]),
        # IOMobileFramebuffer reads the presented canvas (iPhone X 1125x2436).
        node('disp0', [
            ph(),
            ('device_type', cstr('display-subsystem')),
            ('compatible', cstr('tegu,display')),   # AppleCLCD IONameMatch
            ('canvas-width', u32(1125)),
            ('canvas-height', u32(2436)),
        ])])

    # Node names are what the tegu kernel looks up: "tegu-fbcon" (pe_serial.c)
    # and "/tegu-ufs" (tegu_ufs.c). komodo reuses the TEGU board slot.
    fbcon = node('tegu-fbcon', [
        ph(),
        ('reg', u64(FB if qemu else FB_PLACEHOLDER, FB_SIZE)),
        ('width', u32(FB_W)),
        ('height', u32(FB_H)),
        ('stride', u32(FB_STRIDE)),
    ])

    ufs = node('tegu-ufs', [
        ph(),
        ('compatible', cstr('qcom,ufshc')),
        ('reg', u64(UFS, UFS_SIZE)),
    ])

    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1))])

    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold')),
        ('home-button-type', u32(2)),   # D22AP: no Home button
        ('opposed-power-vol-buttons', u32(1)),
    ])

    product_props = d22ap_product()
    product = [node('product', [ph()] + product_props)] if product_props else []

    return node('device-tree', [
        ph(),
        ('compatible', cstr('D22AP') + cstr('iPhone10,3') + cstr('xiaomi,tegu') + cstr('google,komodo') + cstr('google,zumapro') + cstr('AppleARM')),
        ('model', cstr('iPhone10,3')),
        ('target-type', cstr('D22')),
        ('target-sub-type', cstr('D22AP')),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, fbcon, buttons] + product + ([] if qemu else [ufs]))


if __name__ == '__main__':
    args = sys.argv[1:]
    out = next((a for a in args if not a.startswith('--')), None)
    root = build(qemu='--qemu' in args, highmem='--highmem' in args,
                 seed=b'\0' * 64 if '--dump' in args else None)
    blob = serialize(root)
    if out:
        open(out, 'wb').write(blob)
    if '--dump' in args or not out:
        dump(_parse(blob)[0])
    print(f'{out or "(no file)"}: {len(blob)} bytes', file=sys.stderr)
