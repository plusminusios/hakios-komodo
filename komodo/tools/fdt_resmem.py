#!/usr/bin/env python3
"""fdt_resmem.py DTB — print /memory and /reserved-memory regions of a flattened
device tree (no dtc needed). Used to pick a safe physical window for XNU."""
import struct, sys

def parse(b):
    magic, tot, off_struct, off_strings = struct.unpack_from('>IIII', b, 0)
    assert magic == 0xd00dfeed
    def s_at(o):
        e = b.index(b'\0', off_strings + o); return b[off_strings + o:e].decode()
    p = off_struct; path = []; nodes = {}; cells = {}
    while True:
        tok = struct.unpack_from('>I', b, p)[0]; p += 4
        if tok == 1:
            e = b.index(b'\0', p); name = b[p:e].decode(); p = (e + 4) & ~3
            path.append(name); nodes['/'.join(path) or '/'] = {}
        elif tok == 2:
            path.pop()
        elif tok == 3:
            ln, no = struct.unpack_from('>II', b, p); p += 8
            nodes['/'.join(path) or '/'][s_at(no)] = b[p:p + ln]; p = (p + ln + 3) & ~3
        elif tok == 4:
            continue
        elif tok == 9:
            break
    return nodes

def cells_of(nodes, parent):
    pr = nodes.get(parent, {})
    ac = struct.unpack('>I', pr['#address-cells'])[0] if '#address-cells' in pr else 2
    sc = struct.unpack('>I', pr['#size-cells'])[0] if '#size-cells' in pr else 1
    return ac, sc

def regs(raw, ac, sc):
    out = []; step = 4 * (ac + sc)
    for i in range(0, len(raw) - step + 1, step):
        v = struct.unpack('>' + 'I' * (ac + sc), raw[i:i + step])
        a = 0; s = 0
        for x in v[:ac]: a = (a << 32) | x
        for x in v[ac:]: s = (s << 32) | x
        out.append((a, s))
    return out

nodes = parse(open(sys.argv[1], 'rb').read())
root_ac, root_sc = cells_of(nodes, '/')
for path, props in nodes.items():
    if path.split('/')[-1].startswith('memory') and path.count('/') == 1 and 'reg' in props:
        for a, s in regs(props['reg'], root_ac, root_sc):
            print(f"MEMORY   {a:#012x}-{a + s:#012x} ({s >> 20} MiB) {path}")
rm = [p for p in nodes if p.endswith('/reserved-memory')]
for r in rm:
    ac, sc = cells_of(nodes, r)
    items = []
    for path, props in nodes.items():
        if path.startswith(r + '/') and path.count('/') == r.count('/') + 1:
            kind = 'no-map' if 'no-map' in props else 'map'
            if 'reg' in props:
                for a, s in regs(props['reg'], ac, sc):
                    items.append((a, s, path.split('/')[-1], kind))
            else:
                sz = props.get('size'); items.append((None, int.from_bytes(sz, 'big') if sz else 0, path.split('/')[-1], 'dynamic'))
    for a, s, n, k in sorted(items, key=lambda t: (t[0] is None, t[0] or 0)):
        if a is None: print(f"RESERVED dynamic size {s:#x} ({s >> 20} MiB) {n} [{k}]")
        else: print(f"RESERVED {a:#012x}-{a + s:#012x} ({s >> 20:>5} MiB) {n} [{k}]")
