#!/usr/bin/env python3
"""Minimal FDT reader: dump nodes/properties of a .dtb (no deps).
Usage: tools/fdt-read.py FILE [--node /path] [--find substr]"""
import struct, sys, re

def parse(path):
    raw = open(path, "rb").read()
    u32 = lambda o: struct.unpack_from(">I", raw, o)[0]
    ostruct, ostrings = u32(8), u32(12)
    p, end = ostruct, u32(4)
    stack, out = [], {}
    while p < end - 4:
        tok = u32(p)
        if tok not in (1, 2, 3, 4, 9): break
        p += 4
        if tok == 1:  # BEGIN_NODE
            n = raw.index(b"\0", p)
            stack.append(raw[p:n].decode()); p = (n + 1 + 3) & ~3
        elif tok == 2:  # END_NODE
            stack.pop()
        elif tok == 3:  # PROP
            ln, no = u32(p), u32(p + 4); p += 8
            s = ostrings + no; e = raw.index(b"\0", s)
            out.setdefault("/".join(stack), {})[raw[s:e].decode()] = raw[p:p + ln]
            p += (ln + 3) & ~3
        # 4 = NOP
    return out

def fmt(b):
    if not b: return ""
    if len(b) % 4 == 0:
        return " ".join(f"{x:08x}" for x in struct.unpack(f">{len(b)//4}I", b))
    return repr(b)

if __name__ == "__main__":
    props = parse(sys.argv[1])
    find = None
    if "--find" in sys.argv:
        find = sys.argv[sys.argv.index("--find") + 1].lower()
    for k in sorted(props):
        if find:
            hay = k.lower() + " " + (props[k].get("compatible", b"") or b"").decode(errors="replace").lower()
            if find not in hay:
                continue
        print(k)
        for n, v in props[k].items():
            print(f"    {n}: {fmt(v)}")
