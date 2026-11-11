#!/usr/bin/env python3
"""Local check render of an asset pack (independent Python reader of the pack formats, CPU rasterizer).

Reads manifest -> scene -> meshes -> textures exactly as core/io does, decodes one small ASTC mip per texture with
`astcenc -ds`, and draws every opaque/masked instance with per-triangle colour = texture at the triangle's UV centroid
times a fixed directional light. It checks that transforms, UVs, material slots and textures line up; it is not the
look. Camera shots match core/render (kShots in renderer.mm).
Usage: pack_preview.py <pack dir> <out.png> [--shot N] [--size 1600x900]
"""
import math
import os
import struct
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

# (eye, target, horizontal fov deg); keep in sync with kShots in core/render/renderer.mm
SHOTS = [
    ((-9000.0, -7500.0, 4200.0), (0.0, 0.0, 0.0), 80.0),   # same as work/captures/preview/stadium_persp.png
    ((0.0, -7000.0, 1100.0), (0.0, 0.0, 200.0), 90.0),     # behind the blue goal
    ((-6200.0, 0.0, 1500.0), (0.0, 0.0, 300.0), 90.0),     # side line, mid field
    ((0.0, -1500.0, 11000.0), (0.0, 0.0, 0.0), 70.0),      # high overview
]
ASTC_BLOCK = {2: (4, 4), 3: (6, 6), 4: (8, 8)}


def read_strings(d, o, n):
    out = []
    for _ in range(n):
        ln = struct.unpack_from("<H", d, o)[0]
        out.append(d[o + 2:o + 2 + ln].decode())
        o += 2 + ln
    return out, o


def load_scene(pack):
    lines = open(os.path.join(pack, "manifest.txt")).read().split("\n")
    assert lines[0] == "rlios-assetpack 1", lines[0]
    scene = [ln.split() for ln in lines if ln.startswith("scene ")][0][2]
    d = open(os.path.join(pack, scene), "rb").read()
    magic, ver, ns, nt, nm, nme, ni, nsl = struct.unpack_from("<4sIIIIIII", d, 0)
    assert magic == b"RSCN" and ver == 1
    strs, o = read_strings(d, 32, ns)
    tex = []
    for _ in range(nt):
        sid, r, g, b, a = struct.unpack_from("<I4B", d, o); o += 8
        tex.append((strs[sid], (r, g, b, a)))
    mats = []
    for _ in range(nm):
        sid, ti, cr, cg, cb, ca, blend, flags, clip = struct.unpack_from("<Ii4fIIf", d, o); o += 36
        mats.append(dict(name=strs[sid], tex=ti, color=(cr, cg, cb, ca), blend=blend, flags=flags))
    meshes = []
    for _ in range(nme):
        sid, nsec = struct.unpack_from("<II", d, o); o += 8
        meshes.append((strs[sid], nsec))
    inst = []
    for _ in range(ni):
        mi, first = struct.unpack_from("<II", d, o); o += 8
        m = np.frombuffer(d, "<f4", 12, o).astype(np.float64); o += 48
        o += 36  # normal matrix
        flags = struct.unpack_from("<I", d, o)[0]; o += 4
        inst.append((mi, first, m[:9].reshape(3, 3), m[9:12], flags))
    slots = list(struct.unpack_from("<%di" % nsl, d, o))
    return tex, mats, meshes, inst, slots


def load_mesh(path):
    d = open(path, "rb").read()
    magic, ver, nv, ni, ns = struct.unpack_from("<4sIIII", d, 0)
    assert magic == b"RMSH" and ver == 1
    o = 20 + 24
    secs = [struct.unpack_from("<II", d, o + 8 * i) for i in range(ns)]
    o += 8 * ns
    v = np.frombuffer(d, "<f4", nv * 10, o).reshape(nv, 10); o += nv * 40
    idx = np.frombuffer(d, "<u4", ni, o)
    return v, idx, secs


def load_texture_small(path, tmp, want=128):
    d = open(path, "rb").read()
    magic, ver, fmt, w, h, nmip = struct.unpack_from("<4sIIIII", d, 0)
    assert magic == b"RTEX" and ver == 1
    mips = [struct.unpack_from("<IIII", d, 24 + 16 * i) for i in range(nmip)]
    mw, mh, off, size = next((m for m in mips if max(m[0], m[1]) <= want), mips[-1])
    if fmt == 1:
        return np.frombuffer(d, np.uint8, mw * mh * 4, off).reshape(mh, mw, 4).astype(np.float64) / 255
    bx, by = ASTC_BLOCK[fmt]
    hdr = b"\x13\xab\xa1\x5c" + bytes([bx, by, 1]) + mw.to_bytes(3, "little") + mh.to_bytes(3, "little") + \
        (1).to_bytes(3, "little")
    src, dst = os.path.join(tmp, "t.astc"), os.path.join(tmp, "t.png")
    open(src, "wb").write(hdr + d[off:off + size])
    subprocess.run(["astcenc", "-ds", src, dst, "-silent"], check=True)
    return np.asarray(Image.open(dst).convert("RGBA"), np.float64) / 255


def main():
    pack, out = sys.argv[1], sys.argv[2]
    shot = int(sys.argv[sys.argv.index("--shot") + 1]) if "--shot" in sys.argv else 0
    W, H = (int(x) for x in (sys.argv[sys.argv.index("--size") + 1] if "--size" in sys.argv else "1600x900").split("x"))
    tex, mats, meshes, inst, slots = load_scene(pack)
    with tempfile.TemporaryDirectory() as tmp:
        timg = [load_texture_small(os.path.join(pack, p), tmp) for p, _ in tex]
    cache, tris, cols = {}, [], []
    light = np.array([0.3, 0.2, 0.93]); light /= np.linalg.norm(light)
    for mi, first, lin, t, _f in inst:
        if mi not in cache:
            cache[mi] = load_mesh(os.path.join(pack, meshes[mi][0]))
        v, idx, secs = cache[mi]
        for s, (fi, cnt) in enumerate(secs):
            m = mats[slots[first + s]] if slots[first + s] >= 0 else None
            if m is None or m["blend"] >= 2 or m["flags"] & 12:
                continue
            tri = idx[fi:fi + cnt].reshape(-1, 3)
            p = v[tri][..., 0:3].astype(np.float64) @ lin + t
            if m["tex"] >= 0:
                im = timg[m["tex"]]
                uv = v[tri][..., 6:8].mean(1)
                x = (np.mod(uv[:, 0], 1.0) * im.shape[1]).astype(int) % im.shape[1]
                y = (np.mod(uv[:, 1], 1.0) * im.shape[0]).astype(int) % im.shape[0]
                c = im[y, x, :3] * np.array(m["color"][:3])
            else:
                c = np.tile(np.array(m["color"][:3]), (len(tri), 1))
            n = np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0])
            n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-9
            c = c * (0.45 + 0.55 * np.abs(n @ light))[:, None]
            tris.append(p); cols.append(c)
    tris, cols = np.concatenate(tris), np.clip(np.concatenate(cols), 0, 1)
    eye, tgt, fov = (np.array(x) if i < 2 else x for i, x in enumerate(SHOTS[shot]))
    f = tgt - eye; f /= np.linalg.norm(f)
    rgt = np.cross([0, 0, 1.0], f); rgt /= np.linalg.norm(rgt)  # left-handed Z-up: right = up x forward
    up = np.cross(f, rgt)
    rel = tris - eye
    x, y, z = rel @ rgt, rel @ up, rel @ f
    keep = (z > 10).all(1)
    x, y, z, cols = x[keep], y[keep], z[keep], cols[keep]
    fl = W / (2 * math.tan(math.radians(fov / 2)))
    sx, sy = W / 2 + fl * x / z, H / 2 - fl * y / z
    img = np.zeros((H, W, 3)); img[:] = (0.20, 0.40, 0.80)
    zb = np.full((H, W), np.inf)
    for i in np.argsort(-z.mean(1)):
        xs, ys, ds = sx[i], sy[i], z[i]
        x0, x1 = int(max(min(xs.min(), W - 1), 0)), int(min(max(xs.max(), 0), W - 1))
        y0, y1 = int(max(min(ys.min(), H - 1), 0)), int(min(max(ys.max(), 0), H - 1))
        if x1 < x0 or y1 < y0 or (x1 - x0) * (y1 - y0) > 600000:
            continue
        gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        d = (ys[1] - ys[2]) * (xs[0] - xs[2]) + (xs[2] - xs[1]) * (ys[0] - ys[2])
        if abs(d) < 1e-9:
            continue
        a = ((ys[1] - ys[2]) * (gx - xs[2]) + (xs[2] - xs[1]) * (gy - ys[2])) / d
        b = ((ys[2] - ys[0]) * (gx - xs[2]) + (xs[0] - xs[2]) * (gy - ys[2])) / d
        c = 1 - a - b
        msk = (a >= 0) & (b >= 0) & (c >= 0)
        if not msk.any():
            continue
        zz = a * ds[0] + b * ds[1] + c * ds[2]
        sub = zb[y0:y1 + 1, x0:x1 + 1]
        msk &= zz < sub
        sub[msk] = zz[msk]
        img[y0:y1 + 1, x0:x1 + 1][msk] = cols[i]
    Image.fromarray((img * 255).astype(np.uint8)).save(out)
    print("wrote", out, "triangles", len(tris))


if __name__ == "__main__":
    main()
