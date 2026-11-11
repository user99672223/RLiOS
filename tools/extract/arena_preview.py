#!/usr/bin/env python3
"""Assemble a map from its layout JSON (upk_layout.py) + exported glTF meshes and render a quick check image.

The render is a CPU z-buffered rasterizer with flat shading: it is for checking that meshes and placements line up,
not for look development. glTF from umodel is (x, z, y) * 0.01 of Unreal coordinates (ExportGLTF.cpp
TransformPosition), so we undo that. Transform order (UE3 FRotationMatrix, row vectors):
    world = ((v * comp_scale * comp_scale3d) * R(comp_rot) + comp_translation) * drawscale * drawscale3d * R(rot) + location
Usage: arena_preview.py <layout.json> <assets_raw dir> <out.png> [--view top|persp] [--skip-collision]
"""
import json, math, os, struct, sys

import numpy as np
from PIL import Image


def rot_matrix(r):
    p, y, rl = (a * 2 * math.pi / 65536 for a in r)
    sp, cp, sy, cy, sr, cr = math.sin(p), math.cos(p), math.sin(y), math.cos(y), math.sin(rl), math.cos(rl)
    return np.array([[cp * cy, cp * sy, sp],
                     [sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp],
                     [-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp]])


def load_gltf(path):
    g = json.load(open(path))
    base = os.path.dirname(path)
    bufs = [open(os.path.join(base, b["uri"]), "rb").read() for b in g["buffers"]]
    tris = []
    for mesh in g["meshes"]:
        for prim in mesh["primitives"]:
            def acc(i):
                a = g["accessors"][i]
                bv = g["bufferViews"][a["bufferView"]]
                n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[a["type"]]
                dt = {5126: np.float32, 5125: np.uint32, 5123: np.uint16, 5121: np.uint8}[a["componentType"]]
                off = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
                return np.frombuffer(bufs[bv["buffer"]], dt, a["count"] * n, off).reshape(-1, n) if n > 1 else \
                    np.frombuffer(bufs[bv["buffer"]], dt, a["count"], off)
            pos = acc(prim["attributes"]["POSITION"]).astype(np.float64)
            idx = acc(prim["indices"]).astype(np.int64).reshape(-1, 3)
            v = pos[:, [0, 2, 1]] * 100.0  # back to Unreal (x, y, z) in uu
            tris.append(v[idx])
    return np.concatenate(tris) if tris else np.zeros((0, 3, 3))


def main():
    layout, assets, out = sys.argv[1], sys.argv[2], sys.argv[3]
    view = "persp" if "--view" in sys.argv and sys.argv[sys.argv.index("--view") + 1] == "persp" else "top"
    skip_col = "--skip-collision" in sys.argv
    index = {}
    for root, _d, files in os.walk(assets):
        for f in files:
            if f.endswith(".gltf"):
                index.setdefault(f[:-5], os.path.join(root, f))
    cache, all_tris, missing = {}, [], set()
    for r in json.load(open(layout)):
        m = r.get("mesh")
        if not m or r.get("HiddenGame"):
            continue
        name = m.split(".")[-1]
        if skip_col and "Collision" in m:
            continue
        if name not in index:
            missing.add(m)
            continue
        if name not in cache:
            cache[name] = load_gltf(index[name])
        t = cache[name].reshape(-1, 3).copy()
        cs = r.get("comp_scale", 1.0) * np.array(r.get("comp_scale3d", [1, 1, 1])[:3])
        t = t * cs @ rot_matrix(r.get("comp_rotation", [0, 0, 0])) + np.array(r.get("comp_translation", [0, 0, 0])[:3])
        ds = r.get("drawscale", 1.0) * np.array(r.get("drawscale3d", [1, 1, 1])[:3])
        t = t * ds @ rot_matrix(r.get("rotation", [0, 0, 0])) + np.array(r.get("location", [0, 0, 0])[:3])
        all_tris.append(t.reshape(-1, 3, 3))
    tris = np.concatenate(all_tris)
    print("triangles", len(tris), "meshes", len(cache), "missing", len(missing))
    for m in sorted(missing)[:20]:
        print("  missing", m)

    W, H = 1600, 1000
    if view == "top":
        # look down -Z; Unreal is left-handed: screen right = +Y? use x->down, y->right so the field is landscape
        P = np.stack([tris[..., 1], tris[..., 0], tris[..., 2]], -1)
        lim = 6500.0
        sx = (P[..., 0] + lim * 1.6) / (lim * 3.2) * W
        sy = (P[..., 1] + lim) / (lim * 2) * H
        depth = -P[..., 2]
    else:
        eye = np.array([-9000.0, -7500.0, 4200.0]); tgt = np.array([0.0, 0.0, 0.0])
        f = tgt - eye; f /= np.linalg.norm(f)
        rgt = np.cross(f, [0, 0, 1.0]); rgt /= np.linalg.norm(rgt)
        up = np.cross(rgt, f)
        rel = tris - eye
        x, y, z = rel @ rgt, rel @ up, rel @ f
        z = np.maximum(z, 1.0)
        fl = W / (2 * math.tan(math.radians(40)))
        sx = W / 2 + fl * x / z
        sy = H / 2 - fl * y / z
        depth = z
    n = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0])
    n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-9
    light = np.array([0.3, 0.2, 0.93]); light /= np.linalg.norm(light)
    shade = 0.25 + 0.75 * np.abs(n @ light)
    zc = tris[..., 2].mean(1)
    col = np.stack([0.35 + 0.4 * np.clip(zc / 2000, 0, 1), 0.55 + 0.0 * zc, 0.4 + 0.5 * np.clip(zc / 2000, 0, 1)], -1)
    col = np.clip(col * shade[:, None], 0, 1)

    img = np.full((H, W, 3), 0.08)
    zb = np.full((H, W), np.inf)
    for i in np.argsort(-depth.mean(1)):  # painter + zbuffer per pixel (bbox fill with barycentric test)
        xs, ys, ds = sx[i], sy[i], depth[i]
        x0, x1 = int(max(min(xs.min(), W - 1), 0)), int(min(max(xs.max(), 0), W - 1))
        y0, y1 = int(max(min(ys.min(), H - 1), 0)), int(min(max(ys.max(), 0), H - 1))
        if x1 < x0 or y1 < y0 or (x1 - x0) * (y1 - y0) > 400000:
            continue
        gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        d = (ys[1] - ys[2]) * (xs[0] - xs[2]) + (xs[2] - xs[1]) * (ys[0] - ys[2])
        if abs(d) < 1e-9:
            continue
        a = ((ys[1] - ys[2]) * (gx - xs[2]) + (xs[2] - xs[1]) * (gy - ys[2])) / d
        b = ((ys[2] - ys[0]) * (gx - xs[2]) + (xs[0] - xs[2]) * (gy - ys[2])) / d
        c = 1 - a - b
        m = (a >= 0) & (b >= 0) & (c >= 0)
        if not m.any():
            continue
        z = a * ds[0] + b * ds[1] + c * ds[2]
        sub = zb[y0:y1 + 1, x0:x1 + 1]
        m &= z < sub
        sub[m] = z[m]
        img[y0:y1 + 1, x0:x1 + 1][m] = col[i]
    Image.fromarray((img * 255).astype(np.uint8)).save(out)
    print("wrote", out)


if __name__ == "__main__":
    main()
