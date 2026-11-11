#!/usr/bin/env python3
"""Extract a map's placed static meshes (actor transforms + mesh + material overrides) from an unpacked package.

Input is a plain package produced by upk_unpack.py. UE3 (ver 868) tagged-property format:
  tag = Name(int32 index, int32 number), Type(Name), Size(int32), ArrayIndex(int32),
        + StructName(Name) for StructProperty, + 1-byte value for BoolProperty (ver >= 673),
        + EnumName(Name) for ByteProperty (ver >= 633); then Size bytes of value. "None" ends the list.
  Object references are package indices: >0 export (i-1), <0 import (-i-1), 0 null.
  Every export starts with int32 NetIndex; components (ver >= ?) start with TemplateOwnerClass/TemplateName first, so
  the property start is found by trying small offsets and checking the first tag is well formed.
Output JSON: one record per StaticMeshComponent owned by a placed actor (StaticMeshCollectionActor components carry
  their full transform themselves): {actor, actor_class, component, mesh, materials, location, rotation (UU rotator
  ints), drawscale, drawscale3d, comp_translation, comp_rotation, comp_scale, comp_scale3d, flags}. Unset = default
  (0 / 0 / 1). World = actor transform then component transform, UE3 order: scale, rotate (Roll, Pitch, Yaw), translate. Values are exact (floats printed with repr and bits).
Usage: upk_layout.py <unpacked map.upk> <out.json>
"""
import json, struct, sys


class Pkg:
    def __init__(self, path):
        self.d = d = open(path, "rb").read()
        o = 12
        n = struct.unpack_from("<i", d, o)[0]; o += 4 + (n if n >= 0 else -2 * n)
        _f, nc, no, ec, eo, ic, io = struct.unpack_from("<Iiiiiii", d, o)
        self.names = []
        p = no
        for _ in range(nc):
            n = struct.unpack_from("<i", d, p)[0]
            self.names.append(d[p + 4:p + 4 + n - 1].decode("latin-1")); p += 4 + n + 8
        self.imports = []
        p = io
        for _ in range(ic):
            _cp, _cpn, cls, _cn, outer, nm, num = struct.unpack_from("<iiiiiii", d, p); p += 28
            self.imports.append(dict(cls=self.names[cls], outer=outer, name=self.nm(nm, num)))
        self.exports = []
        p = eo
        for _ in range(ec):
            cls, sup, outer, nm, num, arch = struct.unpack_from("<iiiiii", d, p); p += 24
            _flags, size, off, _ef = struct.unpack_from("<Qiqi", d, p); p += 24
            k = struct.unpack_from("<i", d, p)[0]; p += 4 + 4 * k + 20
            self.exports.append(dict(cls=cls, outer=outer, name=self.nm(nm, num), arch=arch, size=size, off=off))

    def nm(self, i, num):
        s = self.names[i]
        return s if num == 0 else "%s_%d" % (s, num - 1)

    def obj(self, ref):
        if ref > 0:
            return self.exports[ref - 1]
        if ref < 0:
            return self.imports[-ref - 1]
        return None

    def full(self, ref):
        parts = []
        while ref:
            o = self.obj(ref)
            parts.append(o["name"]); ref = o["outer"]
        return ".".join(reversed(parts))

    def cls(self, e):
        c = self.obj(e["cls"])
        return c["name"] if c else "Class"

    def fname(self, p):
        i, num = struct.unpack_from("<ii", self.d, p)
        if not (0 <= i < len(self.names)):
            raise ValueError
        return self.nm(i, num)

    def props(self, e):
        for skip in (4, 0, 8, 12, 16, 20):
            try:
                return self._props(e["off"] + skip, e["off"] + e["size"])
            except (ValueError, struct.error, IndexError):
                continue
        return None

    def _props(self, p, end):
        out = {}
        while True:
            if p + 8 > end:
                raise ValueError
            name = self.fname(p); p += 8
            if name == "None":
                return out
            typ = self.fname(p); p += 8
            if not typ.endswith("Property"):
                raise ValueError
            size, idx = struct.unpack_from("<ii", self.d, p); p += 8
            if size < 0 or p + size > end:
                raise ValueError
            sname = None
            if typ == "StructProperty":
                sname = self.fname(p); p += 8
            elif typ == "BoolProperty":
                out[name] = bool(self.d[p]); p += 1
                continue
            elif typ == "ByteProperty":
                p += 8
            val = self.d[p:p + size]; p += size
            out[name if idx == 0 else "%s[%d]" % (name, idx)] = (typ, sname, val)


def f3(v):
    xs = struct.unpack("<3f", v)
    return [float(x) for x in xs] + ["0x%08x" % b for b in struct.unpack("<3I", v)]


def i3(v):
    return list(struct.unpack("<3i", v))


MESH_COMPONENTS = ("StaticMeshComponent", "StaticMeshComponent_TA")


def prop_chain(pk, ref, depth=0):
    """Properties of an export merged over its archetype chain (only archetypes inside this package are readable)."""
    if ref <= 0 or depth > 8:
        return {}
    e = pk.exports[ref - 1]
    base = prop_chain(pk, e["arch"], depth + 1)
    base.update(pk.props(e) or {})
    return base


def main():
    pk = Pkg(sys.argv[1])
    res = []
    for ci, e in enumerate(pk.exports):
        if pk.cls(e) not in MESH_COMPONENTS or e["outer"] <= 0:
            continue
        owner = pk.exports[e["outer"] - 1]
        if not pk.full(e["outer"]).startswith("TheWorld."):
            continue  # only components owned by placed actors (TheWorld.PersistentLevel.<actor>)
        pc = prop_chain(pk, ci + 1)
        pa = prop_chain(pk, e["outer"])
        rec = dict(actor=owner["name"], actor_class=pk.cls(owner), component=e["name"])
        if "StaticMesh" in pc:
            r = struct.unpack("<i", pc["StaticMesh"][2])[0]
            rec["mesh"] = pk.full(r) if r else None
        else:
            rec["mesh"] = None
            rec["mesh_from_archetype"] = pk.full(e["arch"])
        if "Materials" in pc and pc["Materials"][0] == "ArrayProperty":  # int32 count + object refs
            v = pc["Materials"][2]
            n = struct.unpack_from("<i", v, 0)[0]
            rec["materials"] = [pk.full(r) if r else None for r in struct.unpack_from("<%di" % n, v, 4)]
        for src, k, key, conv in ((pa, "Location", "location", f3), (pa, "Rotation", "rotation", i3),
                                  (pa, "DrawScale3D", "drawscale3d", f3), (pc, "Translation", "comp_translation", f3),
                                  (pc, "Rotation", "comp_rotation", i3), (pc, "Scale3D", "comp_scale3d", f3)):
            if k in src:
                rec[key] = conv(src[k][2])
        if "DrawScale" in pa:
            rec["drawscale"] = struct.unpack("<f", pa["DrawScale"][2])[0]
        if "Scale" in pc:
            rec["comp_scale"] = struct.unpack("<f", pc["Scale"][2])[0]
        for flag in ("HiddenGame", "CollideActors", "BlockRigidBody", "CastShadow"):
            if flag in pc and not isinstance(pc[flag], tuple):
                rec[flag] = pc[flag]
        res.append(rec)
    json.dump(res, open(sys.argv[2], "w"), indent=1)
    print(len(res), "mesh components;", sum(1 for r in res if r["mesh"]), "with mesh")


if __name__ == "__main__":
    main()
