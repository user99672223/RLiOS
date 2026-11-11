#!/usr/bin/env python3
"""Build the RLiOS asset pack for one map: layout JSON + umodel glTF meshes + PNG textures -> work/assetpack/.

Deterministic: same inputs + same astcenc version -> byte-identical pack (sorted tables, no timestamps; astcenc
output is independent of its thread count). Formats (all little-endian) are read by core/io/pack.{h,cpp}; keep the
two in sync and bump PACK_VERSION on any change.

  manifest.txt          "rlios-assetpack <version>", "scene <name> <path>", "file <sha256> <size> <path>" lines
  scenes/<map>.scn      RSCN: string table, textures, materials, meshes, instances (world 4x3 + normal 3x3), slots
  meshes/<name>.rmesh   RMSH: sections, vertices {pos[3] uu, nrm[3], uv0[2], uv1[2]} f32, u32 indices
  textures/<name>.rtex  RTEX: ASTC (astcenc, sRGB) full mip chain, or RGBA8 for tiny textures

Coordinates: Unreal units, Unreal axes (X forward, Y right, Z up; left-handed). umodel glTF is (x, z, y) * 0.01 of
Unreal (UEViewer Exporters/ExportGLTF.cpp TransformPosition), undone here. Instance transform, UE3 order (same as
tools/extract/arena_preview.py, row vectors):
    world = ((v * comp_scale * comp_scale3d) * R(comp_rot) + comp_translation) * drawscale * drawscale3d * R(rot)
            + location
composed in float64, stored as float32.

Materials: the layout's per-component `materials[i]` applies to glTF primitive i (umodel exports dummy glTF
materials; checked: counts match for all 415 visible Stadium_P components). Each material is resolved through its
umodel .props.txt parent chain: texture parameter overrides (child wins), then the .mat `Diffuse=` hint; BlendMode,
TwoSided, OpacityMaskClipValue come from the root Material3. Proper material ports come later (CLAUDE.md section 6).

Usage: build_pack.py --layout work/layout/Stadium_P.json --assets work/assets_raw --out work/assetpack
                     [--map Stadium_P] [--astcenc astcenc] [--jobs 3] [--quality -medium]
"""
import argparse
import hashlib
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

PACK_VERSION = 1
MESH_VERSION = 1
TEX_VERSION = 1
SCENE_VERSION = 1

# RTEX formats (core/io/pack.h TexFormat)
FMT_RGBA8_SRGB = 1
FMT_ASTC4_SRGB = 2
FMT_ASTC6_SRGB = 3
FMT_ASTC8_SRGB = 4
ASTC_FMT = {"4x4": FMT_ASTC4_SRGB, "6x6": FMT_ASTC6_SRGB, "8x8": FMT_ASTC8_SRGB}

BLEND = {"BLEND_Opaque": 0, "BLEND_Masked": 1, "BLEND_Translucent": 2, "BLEND_Additive": 3, "BLEND_Modulate": 4}
MAT_TWO_SIDED = 1
MAT_UNLIT = 2
MAT_PLACEHOLDER = 4  # material not found in the exports
MAT_SKIP = 8  # not drawn yet: translucent/additive effect without a usable colour texture
# umodel's `.mat` "Diffuse=" line is a guess; channel-packed/normal/env/curve textures are not base colour.
NOT_BASE_COLOR = re.compile(r"Pack|^ENV|_N$|Normal|^Curve_|LightMap")

# Texture parameter names that carry the base colour, in priority order (from the exported .props.txt files).
DIFFUSE_PARAMS = ["Diffuse", "DiffuseTexture", "BaseColor", "Albedo", "AdTexture", "FieldSkin", "Texture", "Tex"]
COLOR_PARAMS = ["Color", "DiffuseColor", "BaseColor", "Tint", "MainColor"]
# Asset dirs searched first when a name exists in several exported packages.
PREFERRED_DIRS = ["Stadium_P", "Stadium_extra", "Startup"]


def rot_matrix(r):
    """UE3 FRotationMatrix for an int rotator (65536 = 360 deg), row-vector convention."""
    p, y, rl = (a * 2 * math.pi / 65536 for a in r)
    sp, cp, sy, cy, sr, cr = math.sin(p), math.cos(p), math.sin(y), math.cos(y), math.sin(rl), math.cos(rl)
    return np.array([[cp * cy, cp * sy, sp],
                     [sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp],
                     [-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp]])


def vec3(v, default):
    return np.array([float(x) for x in v[:3]]) if v is not None else np.array(default, dtype=np.float64)


def instance_transform(r):
    """Return (L 3x3, T 3) with world = v @ L + T (float64)."""
    cs = float(r.get("comp_scale", 1.0)) * vec3(r.get("comp_scale3d"), [1, 1, 1])
    ds = float(r.get("drawscale", 1.0)) * vec3(r.get("drawscale3d"), [1, 1, 1])
    rc = rot_matrix(r.get("comp_rotation", [0, 0, 0]))
    rd = rot_matrix(r.get("rotation", [0, 0, 0]))
    tc = vec3(r.get("comp_translation"), [0, 0, 0])
    loc = vec3(r.get("location"), [0, 0, 0])
    lin = np.diag(cs) @ rc @ np.diag(ds) @ rd
    t = (tc * ds) @ rd + loc
    return lin, t


# ---------------------------------------------------------------- inputs

class AssetIndex:
    def __init__(self, root):
        self.files = {}
        for d in sorted(os.listdir(root)):
            for base, dirs, fs in os.walk(os.path.join(root, d)):
                dirs.sort()
                for f in sorted(fs):
                    for ext in (".gltf", ".png", ".props.txt", ".mat"):
                        if f.endswith(ext):
                            self.files.setdefault((f[:-len(ext)], ext), []).append((d, os.path.join(base, f)))

    def find(self, name, ext):
        c = self.files.get((name, ext))
        if not c:
            return None
        c = sorted(c, key=lambda t: (PREFERRED_DIRS.index(t[0]) if t[0] in PREFERRED_DIRS else 99, t[1]))
        return c[0][1]


def load_gltf(path):
    g = json.load(open(path))
    base = os.path.dirname(path)
    bufs = [open(os.path.join(base, b["uri"]), "rb").read() for b in g["buffers"]]

    def acc(i):
        a = g["accessors"][i]
        bv = g["bufferViews"][a["bufferView"]]
        n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[a["type"]]
        dt = {5126: np.float32, 5125: np.uint32, 5123: np.uint16, 5121: np.uint8}[a["componentType"]]
        off = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
        stride = bv.get("byteStride", 0)
        if stride and stride != n * np.dtype(dt).itemsize:
            raise ValueError("interleaved glTF not supported: " + path)
        arr = np.frombuffer(bufs[bv["buffer"]], dt, a["count"] * n, off)
        return arr.reshape(-1, n) if n > 1 else arr

    verts, idx, sections = [], [], []
    vbase = 0
    for mesh in g["meshes"]:
        for prim in mesh["primitives"]:
            at = prim["attributes"]
            pos = acc(at["POSITION"]).astype(np.float64)
            cnt = len(pos)
            nrm = acc(at["NORMAL"]).astype(np.float64) if "NORMAL" in at else np.zeros((cnt, 3))
            uv0 = acc(at["TEXCOORD_0"]) if "TEXCOORD_0" in at else np.zeros((cnt, 2), np.float32)
            uv1 = acc(at["TEXCOORD_1"]) if "TEXCOORD_1" in at else uv0
            v = np.zeros((cnt, 10), np.float32)
            v[:, 0:3] = (pos[:, [0, 2, 1]] * 100.0).astype(np.float32)  # glTF -> Unreal uu
            v[:, 3:6] = nrm[:, [0, 2, 1]].astype(np.float32)
            v[:, 6:8] = uv0
            v[:, 8:10] = uv1
            ind = acc(prim["indices"]).astype(np.uint32).reshape(-1, 3)
            ind = ind[:, [0, 2, 1]]  # axis swap mirrors the mesh: keep front faces' winding
            sections.append((sum(len(i) for i in idx) * 3, len(ind) * 3))
            idx.append(ind + vbase)
            verts.append(v)
            vbase += cnt
    return np.concatenate(verts), np.concatenate(idx).reshape(-1), sections


def parse_props(path):
    s = open(path, errors="replace").read()
    out = {"parent": None, "tex": {}, "vec": {}, "blend": None, "two_sided": None, "clip": None, "unlit": None}
    m = re.search(r"^Parent = \w+'([^']+)'", s, re.M)
    if m:
        out["parent"] = m.group(1).split(".")[-1]
    for val, name in re.findall(r"ParameterValue = Texture\w*'([^']+)'\s*\n\s*ParameterName = (\w+)", s):
        out["tex"][name] = val.split(".")[-1]
    for r, g, b, a, name in re.findall(
            r"ParameterValue = \{ R=([-\d.e]+), G=([-\d.e]+), B=([-\d.e]+), A=([-\d.e]+) \}\s*\n\s*ParameterName = (\w+)", s):
        out["vec"][name] = (float(r), float(g), float(b), float(a))
    m = re.search(r"^BlendMode = (\w+)", s, re.M)
    if m:
        out["blend"] = m.group(1)
    m = re.search(r"^TwoSided = (\w+)", s, re.M)
    if m:
        out["two_sided"] = m.group(1) == "true"
    m = re.search(r"^OpacityMaskClipValue = ([\d.e-]+)", s, re.M)
    if m:
        out["clip"] = float(m.group(1))
    m = re.search(r"^LightingModel = (\w+)", s, re.M)
    if m:
        out["unlit"] = m.group(1) == "MLM_Unlit"
    return out


def parse_mat_hint(path):
    for line in open(path, errors="replace"):
        if line.startswith("Diffuse="):
            return line.strip().split("=", 1)[1]
    return None


def resolve_material(name, index):
    """-> dict(texture name or None, color, blend, flags, clip)."""
    chain, n = [], name
    while n and n not in [c[0] for c in chain] and len(chain) < 16:
        p = index.find(n, ".props.txt")
        if not p:
            break
        chain.append((n, parse_props(p)))
        n = chain[-1][1]["parent"]
    if not chain:
        return dict(texture=None, color=(0.5, 0.5, 0.5, 1.0), blend=0, flags=MAT_PLACEHOLDER, clip=0.333)
    tex, vec = {}, {}
    for _n, pr in reversed(chain):  # root first, children override
        tex.update(pr["tex"])
        vec.update(pr["vec"])
    root = chain[-1][1]
    texture = None
    for k in DIFFUSE_PARAMS:
        if tex.get(k) and index.find(tex[k], ".png"):
            texture = tex[k]
            break
    if texture is None:
        for n, _pr in chain:
            mp = index.find(n, ".mat")
            hint = parse_mat_hint(mp) if mp else None
            if hint and index.find(hint, ".png") and not NOT_BASE_COLOR.search(hint):
                texture = hint
                break
    color = (1.0, 1.0, 1.0, 1.0) if texture else (0.5, 0.5, 0.5, 1.0)
    for k in COLOR_PARAMS:
        if k in vec:
            color = vec[k]
            break
    flags = 0
    if root["two_sided"]:
        flags |= MAT_TWO_SIDED
    if root["unlit"]:
        flags |= MAT_UNLIT
    blend = BLEND.get(root["blend"] or "BLEND_Opaque", 0)
    if blend == 1 and texture and texture.startswith("FieldSkin"):
        # Field skin alpha is the glow-line mask, not opacity (FieldSkin_FutureGrid_1x1_D: RGB = field colour,
        # A = line mask, mostly 0); with the masked clip of 0.05 the whole field would be discarded.
        blend = 0
    if blend in (2, 3) and not texture:
        flags |= MAT_SKIP
    clip = root["clip"] if root["clip"] is not None else 0.333
    return dict(texture=texture, color=color, blend=blend, flags=flags, clip=clip)


# ---------------------------------------------------------------- outputs

def write_mesh(path, verts, idx, sections):
    lo = verts[:, 0:3].min(0) if len(verts) else np.zeros(3, np.float32)
    hi = verts[:, 0:3].max(0) if len(verts) else np.zeros(3, np.float32)
    with open(path, "wb") as f:
        f.write(struct.pack("<4sIIII", b"RMSH", MESH_VERSION, len(verts), len(idx), len(sections)))
        f.write(struct.pack("<6f", *lo, *hi))
        for first, count in sections:
            f.write(struct.pack("<II", first, count))
        f.write(np.ascontiguousarray(verts, "<f4").tobytes())
        f.write(np.ascontiguousarray(idx, "<u4").tobytes())


def mip_chain(img):
    # Per channel: PIL premultiplies RGBA when resizing, which turns colour under alpha 0 black. Alpha is not always
    # opacity here (field skin line masks), so filter RGB and A independently.
    mips = [img]
    while mips[-1].size != (1, 1):
        w, h = mips[-1].size
        size = (max(1, w // 2), max(1, h // 2))
        mips.append(Image.merge("RGBA", [c.resize(size, Image.BOX) for c in mips[-1].split()]))
    return mips


def block_for(name):
    # CLAUDE.md section 6: 4x4 for close-up detail (field markings, ads), 6x6 general.
    if re.search(r"FieldSkin|Ad|Advert|Flag", name):
        return "4x4"
    return "6x6"


def astc_compress(astcenc, png, out, block, quality, jobs):
    subprocess.run([astcenc, "-cs", png, out, block, quality, "-j", str(jobs), "-silent"], check=True)
    data = open(out, "rb").read()
    if data[:4] != b"\x13\xab\xa1\x5c":
        raise ValueError("unexpected astcenc output header")
    return data[16:]  # strip the 16-byte .astc file header, keep the blocks


def write_texture(path, src_png, astcenc, quality, jobs, tmp):
    img = Image.open(src_png).convert("RGBA")
    w, h = img.size
    mips = mip_chain(img)
    avg = tuple(int(round(c)) for c in np.asarray(mips[-1], np.float64).reshape(-1, 4)[0])
    name = os.path.basename(src_png)[:-4]
    if w < 8 or h < 8:
        fmt, blobs = FMT_RGBA8_SRGB, [np.asarray(m, np.uint8).tobytes() for m in mips]
    else:
        block = block_for(name)
        fmt, blobs = ASTC_FMT[block], []
        for i, m in enumerate(mips):
            p = os.path.join(tmp, "mip%d.png" % i)
            m.save(p)
            blobs.append(astc_compress(astcenc, p, os.path.join(tmp, "mip%d.astc" % i), block, quality, jobs))
    with open(path, "wb") as f:
        f.write(struct.pack("<4sIIIII", b"RTEX", TEX_VERSION, fmt, w, h, len(mips)))
        off = 24 + 16 * len(mips)
        for m, b in zip(mips, blobs):
            f.write(struct.pack("<IIII", m.size[0], m.size[1], off, len(b)))
            off += len(b)
        for b in blobs:
            f.write(b)
    return avg


class Strings:
    def __init__(self):
        self.items, self.ids = [], {}

    def id(self, s):
        if s not in self.ids:
            self.ids[s] = len(self.items)
            self.items.append(s)
        return self.ids[s]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def skip_component(r):
    m = r.get("mesh")
    if not m or r.get("HiddenGame"):
        return True
    mats = r.get("materials", [])
    return "Collision" in m or (mats and all(x and "Collision_Mat" in x for x in mats))


def build(layout_path, assets, out, map_name, astcenc, quality, jobs):
    index = AssetIndex(assets)
    layout = json.load(open(layout_path))
    comps, missing_meshes = [], set()
    for r in layout:
        if skip_component(r):
            continue
        mname = r["mesh"].split(".")[-1]
        if not index.find(mname, ".gltf"):
            missing_meshes.add(r["mesh"])
            continue
        comps.append(r)

    mesh_names = sorted({r["mesh"].split(".")[-1] for r in comps})
    mat_names = sorted({(m or "None") for r in comps for m in r.get("materials", [])})
    mats = {n: resolve_material(n.split(".")[-1], index) for n in mat_names}
    tex_names = sorted({m["texture"] for m in mats.values() if m["texture"]})

    if os.path.isdir(out):
        shutil.rmtree(out)
    for d in ("scenes", "meshes", "textures"):
        os.makedirs(os.path.join(out, d))

    mesh_sections = {}
    for n in mesh_names:
        v, i, s = load_gltf(index.find(n, ".gltf"))
        write_mesh(os.path.join(out, "meshes", n + ".rmesh"), v, i, s)
        mesh_sections[n] = len(s)

    tex_avg = {}
    with tempfile.TemporaryDirectory(prefix="rlios_pack_") as tmp:
        for k, n in enumerate(tex_names):
            tex_avg[n] = write_texture(os.path.join(out, "textures", n + ".rtex"), index.find(n, ".png"), astcenc,
                                       quality, jobs, tmp)
            print("  texture %d/%d %s" % (k + 1, len(tex_names), n), file=sys.stderr)

    # Scene
    st = Strings()
    tex_id = {n: i for i, n in enumerate(tex_names)}
    mat_id = {n: i for i, n in enumerate(mat_names)}
    mesh_id = {n: i for i, n in enumerate(mesh_names)}
    body = bytearray()
    for n in tex_names:
        body += struct.pack("<I4B", st.id("textures/%s.rtex" % n), *tex_avg[n])
    for n in mat_names:
        m = mats[n]
        body += struct.pack("<Ii4fIIf", st.id(n), tex_id[m["texture"]] if m["texture"] else -1, *m["color"],
                            m["blend"], m["flags"], m["clip"])
    for n in mesh_names:
        body += struct.pack("<II", st.id("meshes/%s.rmesh" % n), mesh_sections[n])
    slots = []
    for r in comps:
        mname = r["mesh"].split(".")[-1]
        lin, t = instance_transform(r)
        nrm = np.linalg.inv(lin).T  # row-vector normal transform: n' = n @ inv(L).T
        mlist = r.get("materials", [])
        first = len(slots)
        for s in range(mesh_sections[mname]):
            slots.append(mat_id[mlist[s] or "None"] if s < len(mlist) else mat_id.setdefault("None", -1))
        flags = 1 if np.linalg.det(lin) < 0 else 0  # mirrored
        body += struct.pack("<II", mesh_id[mname], first)
        body += np.asarray(np.concatenate([lin.reshape(-1), t]), "<f4").tobytes()
        body += np.asarray(nrm.reshape(-1), "<f4").tobytes()
        body += struct.pack("<I", flags)
    for s in slots:
        body += struct.pack("<i", s)
    head = struct.pack("<4sIIIIII", b"RSCN", SCENE_VERSION, len(st.items), len(tex_names), len(mat_names),
                       len(mesh_names), len(comps)) + struct.pack("<I", len(slots))
    strs = bytearray()
    for s in st.items:
        b = s.encode("utf-8")
        strs += struct.pack("<H", len(b)) + b
    scene_rel = "scenes/%s.scn" % map_name
    with open(os.path.join(out, scene_rel), "wb") as f:
        f.write(head + strs + body)

    files = []
    for base, dirs, fs in os.walk(out):
        dirs.sort()
        for f in sorted(fs):
            p = os.path.join(base, f)
            files.append((os.path.relpath(p, out), p))
    files.sort()
    with open(os.path.join(out, "manifest.txt"), "w", newline="\n") as f:
        f.write("rlios-assetpack %d\n" % PACK_VERSION)
        f.write("scene %s %s\n" % (map_name, scene_rel))
        for rel, p in files:
            f.write("file %s %d %s\n" % (sha256(p), os.path.getsize(p), rel))
    stats = dict(instances=len(comps), meshes=len(mesh_names), materials=len(mat_names), textures=len(tex_names),
                 missing_meshes=sorted(missing_meshes),
                 untextured=[n for n in mat_names if not mats[n]["texture"]],
                 placeholder_materials=[n for n in mat_names if mats[n]["flags"] & MAT_PLACEHOLDER])
    return stats


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--layout", required=True)
    ap.add_argument("--assets", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--map", default=None)
    ap.add_argument("--astcenc", default="astcenc")
    ap.add_argument("--quality", default="-medium")
    ap.add_argument("--jobs", type=int, default=3)
    a = ap.parse_args()
    map_name = a.map or os.path.basename(a.layout).split(".")[0]
    stats = build(a.layout, a.assets, a.out, map_name, a.astcenc, a.quality, a.jobs)
    print(json.dumps(stats, indent=1))


if __name__ == "__main__":
    main()
