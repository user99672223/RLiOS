#!/usr/bin/env python3
"""Dump placed gameplay actors (boost pads, goals, player starts, ...) of an unpacked map package with every
tagged property, merged over the in-package archetype chain. Reuses the package reader of upk_layout.py.

Output JSON: [{actor, class, archetype, props:{name: value}, components:[{name, class, archetype, props}]}].
Values: floats/vectors exact (repr + bit pattern), rotators as UU ints, object refs as full names,
other structs/arrays as hex.
Usage: upk_actors.py <unpacked map.upk> <out.json> [ClassSubstring ...]   (no filter = class histogram only)
"""
import collections, json, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from upk_layout import Pkg, prop_chain  # noqa: E402


def conv(pk, v):
    if not isinstance(v, tuple):
        return v
    typ, sname, val = v
    if typ == "FloatProperty":
        return [struct.unpack("<f", val)[0], "0x%08x" % struct.unpack("<I", val)[0]]
    if typ == "IntProperty":
        return struct.unpack("<i", val)[0]
    if typ == "ByteProperty":
        return val[0] if len(val) == 1 else pk.fname_bytes(val)
    if typ in ("ObjectProperty", "ComponentProperty", "ClassProperty"):
        r = struct.unpack("<i", val)[0]
        return pk.full(r) if r else None
    if typ == "NameProperty":
        i, num = struct.unpack("<ii", val)
        return pk.nm(i, num)
    if typ == "StructProperty" and sname == "Vector":
        return [float(x) for x in struct.unpack("<3f", val)] + ["0x%08x" % b for b in struct.unpack("<3I", val)]
    if typ == "StructProperty" and sname == "Rotator":
        return list(struct.unpack("<3i", val))
    return {"type": typ, "struct": sname, "hex": val.hex()}


def fname_bytes(self, val):
    i, num = struct.unpack("<ii", val)
    return self.nm(i, num)


Pkg.fname_bytes = fname_bytes


def main():
    pk = Pkg(sys.argv[1])
    filt = sys.argv[3:]
    hist = collections.Counter()
    kids = collections.defaultdict(list)
    for i, e in enumerate(pk.exports):
        if e["outer"] > 0:
            kids[e["outer"]].append(i + 1)
    res = []
    for i, e in enumerate(pk.exports):
        if e["outer"] <= 0 or pk.full(e["outer"]) != "TheWorld.PersistentLevel":
            continue
        c = pk.cls(e)
        hist[c] += 1
        if not any(f in c for f in filt):
            continue
        rec = dict(actor=e["name"], archetype=pk.full(e["arch"]) if e["arch"] else None)
        rec["class"] = c
        rec["props"] = {k: conv(pk, v) for k, v in prop_chain(pk, i + 1).items()}
        rec["components"] = [dict(name=pk.exports[k - 1]["name"], archetype=pk.full(pk.exports[k - 1]["arch"])
                                  if pk.exports[k - 1]["arch"] else None, props={
                                      a: conv(pk, b) for a, b in prop_chain(pk, k).items()})
                             for k in kids[i + 1]]
        for comp, k in zip(rec["components"], kids[i + 1]):
            comp["class"] = pk.cls(pk.exports[k - 1])
        res.append(rec)
    json.dump(res, open(sys.argv[2], "w"), indent=1)
    for c, n in sorted(hist.items()):
        print("%5d %s" % (n, c))
    print(len(res), "actors written")


if __name__ == "__main__":
    main()
