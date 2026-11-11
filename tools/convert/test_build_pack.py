#!/usr/bin/env python3
"""Linux tests for the asset pack converter (synthetic inputs only; no game data).
Run: python3 -m unittest tools/convert/test_build_pack.py   (needs numpy, Pillow, astcenc on PATH)"""
import base64
import json
import os
import shutil
import struct
import sys
import tempfile
import unittest

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_pack  # noqa: E402
import pack_preview  # noqa: E402


def write_gltf(path):
    """Two primitives (sections): a triangle and a quad, glTF axes (x, z, y) * 0.01 of Unreal."""
    pos = np.array([[0, 0, 0], [1, 0, 0], [0, 0, 1], [0, 1, 0], [1, 1, 0], [1, 1, 1], [0, 1, 1]], np.float32)
    nrm = np.tile(np.array([0, 1, 0], np.float32), (7, 1))
    uv = np.array([[0, 0], [1, 0], [0, 1], [0, 0], [1, 0], [1, 1], [0, 1]], np.float32)
    i0 = np.array([0, 1, 2], np.uint16)
    i1 = np.array([0, 1, 2, 0, 2, 3], np.uint16)
    blobs = [pos[:3].tobytes(), nrm[:3].tobytes(), uv[:3].tobytes(), i0.tobytes(),
             pos[3:].tobytes(), nrm[3:].tobytes(), uv[3:].tobytes(), i1.tobytes()]
    buf, views, off = b"", [], 0
    for b in blobs:
        views.append({"buffer": 0, "byteOffset": off, "byteLength": len(b)})
        buf += b + b"\0" * ((-len(b)) % 4)
        off = len(buf)
    acc = [
        {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
        {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC2"},
        {"bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR"},
        {"bufferView": 4, "componentType": 5126, "count": 4, "type": "VEC3"},
        {"bufferView": 5, "componentType": 5126, "count": 4, "type": "VEC3"},
        {"bufferView": 6, "componentType": 5126, "count": 4, "type": "VEC2"},
        {"bufferView": 7, "componentType": 5123, "count": 6, "type": "SCALAR"},
    ]
    g = {"buffers": [{"uri": os.path.basename(path)[:-5] + ".bin", "byteLength": len(buf)}], "bufferViews": views,
         "accessors": acc, "materials": [{"name": "dummy_material_0"}, {"name": "dummy_material_1"}],
         "meshes": [{"primitives": [
             {"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 0},
             {"attributes": {"POSITION": 4, "NORMAL": 5, "TEXCOORD_0": 6}, "indices": 7, "material": 1}]}]}
    json.dump(g, open(path, "w"))
    open(path[:-5] + ".bin", "wb").write(buf)


def make_assets(root):
    pkg = os.path.join(root, "Stadium_P")
    for d in ("StaticMesh3", "Texture2D", "Material3", "MaterialInstanceConstant"):
        os.makedirs(os.path.join(pkg, d))
    write_gltf(os.path.join(pkg, "StaticMesh3", "Box.gltf"))
    img = np.zeros((40, 24, 4), np.uint8)
    img[..., 0] = 200; img[..., 1] = 100; img[..., 2] = 50; img[..., 3] = 0  # colour under alpha 0
    Image.fromarray(img, "RGBA").save(os.path.join(pkg, "Texture2D", "Wall_D.png"))
    open(os.path.join(pkg, "Material3", "Base_Mat.props.txt"), "w").write(
        "TwoSided = true\nBlendMode = BLEND_Opaque (0)\n")
    open(os.path.join(pkg, "Material3", "Base_Mat.mat"), "w").write("Diffuse=Wall_D\n")
    open(os.path.join(pkg, "Material3", "Glow_Mat.props.txt"), "w").write("BlendMode = BLEND_Additive (3)\n")
    open(os.path.join(pkg, "MaterialInstanceConstant", "Tinted_MIC.props.txt"), "w").write(
        "Parent = Material3'Pkg.Base_Mat'\nVectorParameterValues[1] =\n{\n    VectorParameterValues[0] =\n    {\n"
        "        ParameterInfo = \n        ParameterValue = { R=0.25, G=0.5, B=0.75, A=1 }\n"
        "        ParameterName = Color\n    }\n}\n")


LAYOUT = [
    {"mesh": "Pkg.Box", "materials": ["Pkg.Tinted_MIC", "Pkg.Glow_Mat"], "location": [100.0, -50.0, 10.0],
     "rotation": [1000, 16384, -3000], "drawscale": 2.0, "drawscale3d": [1.0, 0.5, 3.0],
     "comp_translation": [5.0, 6.0, 7.0, "0x0", "0x0", "0x0"], "comp_rotation": [0, 8192, 0],
     "comp_scale3d": [1.5, 1.0, -1.0, "0x0", "0x0", "0x0"]},
    {"mesh": "Pkg.Box", "materials": ["Pkg.Base_Mat", "Pkg.Missing_Mat"]},
    {"mesh": "Pkg.Box", "materials": ["Pkg.Base_Mat"], "HiddenGame": True},
    {"mesh": "CollisionMeshes.Box", "materials": ["CollisionMeshes.Collision_Mat"]},
    {"mesh": "Pkg.NotExported", "materials": []},
]


class ConverterTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("astcenc"):
            raise unittest.SkipTest("astcenc not installed")
        cls.tmp = tempfile.mkdtemp(prefix="rlios_conv_test_")
        cls.assets = os.path.join(cls.tmp, "assets")
        make_assets(cls.assets)
        cls.layout = os.path.join(cls.tmp, "Test_P.json")
        json.dump(LAYOUT, open(cls.layout, "w"))
        cls.out = os.path.join(cls.tmp, "pack")
        cls.stats = build_pack.build(cls.layout, cls.assets, cls.out, "Test_P", "astcenc", "-fast", 2)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_stats(self):
        self.assertEqual(self.stats["instances"], 2)
        self.assertEqual(self.stats["missing_meshes"], ["Pkg.NotExported"])
        self.assertEqual(self.stats["placeholder_materials"], ["Pkg.Missing_Mat"])

    def test_deterministic(self):
        out2 = os.path.join(self.tmp, "pack2")
        build_pack.build(self.layout, self.assets, out2, "Test_P", "astcenc", "-fast", 1)
        a = open(os.path.join(self.out, "manifest.txt")).read()
        self.assertEqual(a, open(os.path.join(out2, "manifest.txt")).read())
        self.assertIn("rlios-assetpack 1\nscene Test_P scenes/Test_P.scn\n", a)

    def test_mesh_roundtrip(self):
        v, idx, secs = pack_preview.load_mesh(os.path.join(self.out, "meshes", "Box.rmesh"))
        self.assertEqual(secs, [(0, 3), (3, 6)])
        self.assertEqual(len(v), 7)
        # glTF (1, 0, 0) -> uu (100, 0, 0); glTF (0, 0, 1) -> uu (0, 100, 0); glTF y -> uu z
        np.testing.assert_array_equal(v[1, 0:3], [100, 0, 0])
        np.testing.assert_array_equal(v[2, 0:3], [0, 100, 0])
        np.testing.assert_array_equal(v[3, 0:3], [0, 0, 100])
        np.testing.assert_array_equal(v[0, 3:6], [0, 0, 1])
        self.assertEqual(list(idx[:3]), [0, 2, 1])  # winding flipped with the axis swap
        self.assertEqual(list(idx[3:6]), [3, 5, 4])  # second section rebased onto the shared vertex buffer

    def test_transform_matches_preview_formula(self):
        tex, mats, meshes, inst, slots = pack_preview.load_scene(self.out)
        r = LAYOUT[0]
        v = np.array([[12.0, -34.0, 56.0], [1.0, 2.0, 3.0]])
        ap = build_pack  # same rot_matrix as tools/extract/arena_preview.py
        cs = 1.0 * np.array([1.5, 1.0, -1.0])
        ds = 2.0 * np.array([1.0, 0.5, 3.0])
        want = ((v * cs) @ ap.rot_matrix(r["comp_rotation"]) + [5.0, 6.0, 7.0]) * ds @ ap.rot_matrix(r["rotation"]) \
            + r["location"]
        _mi, _first, lin, t, flags = inst[0]
        got = v @ lin + t
        np.testing.assert_allclose(got, want, rtol=1e-6, atol=1e-3)
        self.assertEqual(flags & 1, 1)  # negative comp_scale3d.z: mirrored
        self.assertEqual(inst[1][4] & 1, 0)

    def test_materials(self):
        tex, mats, meshes, inst, slots = pack_preview.load_scene(self.out)
        by = {m["name"]: m for m in mats}
        tinted = by["Pkg.Tinted_MIC"]
        self.assertEqual(tex[tinted["tex"]][0], "textures/Wall_D.rtex")  # inherited .mat Diffuse hint
        self.assertEqual(tinted["color"], (0.25, 0.5, 0.75, 1.0))
        self.assertEqual(tinted["flags"] & build_pack.MAT_TWO_SIDED, build_pack.MAT_TWO_SIDED)
        self.assertEqual(by["Pkg.Glow_Mat"]["blend"], 3)
        self.assertTrue(by["Pkg.Glow_Mat"]["flags"] & build_pack.MAT_SKIP)
        self.assertTrue(by["Pkg.Missing_Mat"]["flags"] & build_pack.MAT_PLACEHOLDER)
        self.assertEqual([mats[s]["name"] for s in slots], ["Pkg.Tinted_MIC", "Pkg.Glow_Mat", "Pkg.Base_Mat",
                                                            "Pkg.Missing_Mat"])

    def test_texture(self):
        d = open(os.path.join(self.out, "textures", "Wall_D.rtex"), "rb").read()
        magic, ver, fmt, w, h, nmip = struct.unpack_from("<4sIIIII", d)
        self.assertEqual((magic, ver, fmt, w, h, nmip), (b"RTEX", 1, build_pack.FMT_ASTC6_SRGB, 24, 40, 6))
        mips = [struct.unpack_from("<IIII", d, 24 + 16 * i) for i in range(nmip)]
        self.assertEqual([(m[0], m[1]) for m in mips], [(24, 40), (12, 20), (6, 10), (3, 5), (1, 2), (1, 1)])
        self.assertEqual(mips[0][3], 4 * 7 * 16)  # ceil(24/6) * ceil(40/6) blocks of 16 bytes
        self.assertEqual(mips[-1][2] + mips[-1][3], len(d))
        tmp = os.path.join(self.tmp, "dec")
        os.makedirs(tmp, exist_ok=True)
        small = pack_preview.load_texture_small(os.path.join(self.out, "textures", "Wall_D.rtex"), tmp, want=4)
        # colour under zero alpha survives mip filtering (no premultiply)
        np.testing.assert_allclose(small[..., :3].mean((0, 1)), [200 / 255, 100 / 255, 50 / 255], atol=0.03)


if __name__ == "__main__":
    unittest.main()
