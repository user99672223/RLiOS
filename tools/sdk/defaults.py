#!/usr/bin/env python3
"""Print class-default property values from the hook's SDK dump (sdk.txt + defaults.bin).

sdk.txt lines: S=struct/class (kind, name, super, size), P=property (name, type, offset, size, dim, flags, extra),
D=defaults blob (offset, size in defaults.bin). Scalars are decoded (float with bit pattern); structs are decoded
recursively through their S records when known, else shown as hex.
Usage: defaults.py <sdk dir> <Package.Class> [name-regex]      (walks the super chain, most derived first)
"""
import re, struct, sys


def load(sdk):
    classes, cur = {}, None
    for line in open(sdk + "/sdk.txt", encoding="latin-1"):
        f = line.rstrip("\n").split("\t")
        if f[0] == "S":
            cur = dict(name=f[2], sup=f[3], size=int(f[4], 16), props=[], blob=None)
            classes[f[2]] = cur
        elif f[0] == "P" and cur is not None:
            cur["props"].append(dict(name=f[1], type=f[2], off=int(f[3], 16), size=int(f[4], 16), dim=int(f[5]),
                                     extra=f[7] if len(f) > 7 else ""))
        elif f[0] == "D" and cur is not None:
            cur["blob"] = (int(f[1]), int(f[2]))
    return classes


def decode(classes, p, b, depth=0):
    t = p["type"]
    if t == "FloatProperty":
        return "%r (0x%08x)" % (struct.unpack("<f", b)[0], struct.unpack("<I", b)[0])
    if t == "IntProperty":
        return str(struct.unpack("<i", b)[0])
    if t == "ByteProperty":
        return str(b[0])
    if t == "BoolProperty":
        m = int(p["extra"].split("=")[1], 16) if "mask=" in p["extra"] else 1
        return str(bool(struct.unpack("<I", b)[0] & m))
    if t == "StructProperty" and p["extra"] in classes and depth < 4:
        s, parts = classes[p["extra"]], []
        chain = []
        while s:
            chain.append(s)
            s = classes.get(s["sup"])
        for s in reversed(chain):
            for q in s["props"]:
                if q["off"] + q["size"] <= len(b):
                    parts.append("%s=%s" % (q["name"], decode(classes, q, b[q["off"]:q["off"] + q["size"]], depth + 1)))
        return "{" + ", ".join(parts) + "}"
    return b.hex()


def main():
    sdk, cname = sys.argv[1], sys.argv[2]
    rx = re.compile(sys.argv[3], re.I) if len(sys.argv) > 3 else None
    classes = load(sdk)
    blob = open(sdk + "/defaults.bin", "rb").read()
    top = classes[cname]
    boff, bsize = top["blob"]
    data = blob[boff:boff + bsize]
    c = top
    while c:
        print("# %s (size 0x%x)" % (c["name"], c["size"]))
        for p in c["props"]:
            if rx and not rx.search(p["name"]):
                continue
            es = p["size"]
            for i in range(p["dim"]):
                b = data[p["off"] + i * es:p["off"] + (i + 1) * es]
                if len(b) < es:
                    continue
                nm = p["name"] if p["dim"] == 1 else "%s[%d]" % (p["name"], i)
                print("%-40s %-16s @0x%03x  %s" % (nm, p["type"].replace("Property", ""), p["off"] + i * es,
                                                  decode(classes, p, b)))
        c = classes.get(c["sup"])


if __name__ == "__main__":
    main()
