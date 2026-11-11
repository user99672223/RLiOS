#!/usr/bin/env python3
"""Dump the arena's static triangle-mesh collision from the running (hooked, offline) game.

Reads the live Bullet world through the hook's `dumpmem` command and writes one RLCOL1 file with
every btBvhTriangleMeshShape object in world order: world transform, the quantized BVH exactly as
the game built it, vertices, indices and the per-triangle btTriangleInfo (edge angles and flags).
The sim needs the game's own BVH and info map, not just the triangles, because node order decides
contact order and the edge angles feed btAdjustInternalEdgeContacts.

The output is game data: it goes to $RLIOS_WORK/collision/ and is never committed.

Usage (game running in the wanted arena, see run.sh / record.py --keep):
    python3 dump_collision.py [out-file]

File format (little endian):
    char[8] "RLCOL1\\0\\0", u32 meshCount
    per mesh:
      f32[12] world transform (basis rows 3x3, then origin)
      f32[9]  bvh aabb min, aabb max, quantization
      u32     triangleCount, vertexCount, nodeCount
      u32[3*triangleCount]  indices
      f32[3*vertexCount]    vertices (mesh space)
      nodeCount * 16 bytes  btQuantizedBvhNode (u16 min[3], u16 max[3], i32 escapeOrTriangle)
      triangleCount * 16    i32 flags, f32 edge angle V0V1, V1V2, V2V0 (flags = -1: no info)

Layout evidence (offsets measured in the live process, classes are Bullet 2.82):
  world: collision object array count +0xC, data +0x18. object: transform +0x10, shape +0xD0.
  shape: type +0x8 (21 = triangle mesh), mesh interface +0x40, bvh +0x48, info map +0x50.
  mesh interface: scaling +0x10, indexed mesh array +0x30. indexed mesh: triangle count +0,
  index base +8, index stride +0x10, vertex count +0x14, vertex base +0x18, vertex stride +0x20.
  bvh: aabb min +0x10, max +0x20, quantization +0x30, node count +0x44, quantized flag +0x48,
  nodes +0xC0, traversal mode +0xD0. info map: value count +0x4C, values +0x58, keys +0x78.
"""
import os
import struct
import sys

import record

WORLD_CHAIN_RVA = 0x24846B8  # global -> +0x200 -> +0xB0 -> [0] = scene, scene +0x20 = btWorld

_n = [0]


def read(addr, n):
    _n[0] += 1
    name = "col_%d_%d.bin" % (os.getpid(), _n[0])
    path = os.path.join(record.SESS, name)
    record.send(["dumpmem %x %d %s" % (addr, n, name)], wait=0.1)
    data = open(path, "rb").read() if os.path.exists(path) else b""
    if os.path.exists(path):
        os.remove(path)
    if len(data) != n:
        raise RuntimeError("short read at %x (%d of %d)" % (addr, len(data), n))
    return data


def u64(b, o):
    return struct.unpack_from("<Q", b, o)[0]


def i32(b, o):
    return struct.unpack_from("<i", b, o)[0]


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.environ["RLIOS_WORK"], "collision", "arena.rlcol")
    base = int(open(os.path.join(record.SESS, "hook.log")).readline().split("base=")[1].split()[0],
               16)
    g = u64(read(base + WORLD_CHAIN_RVA, 8), 0)
    arr = u64(read(u64(read(g + 0x200, 8), 0) + 0xB0, 8), 0)
    scene = u64(read(arr, 8), 0)
    world = u64(read(scene + 0x20, 8), 0)
    wh = read(world, 0x20)
    count = i32(wh, 0xC)
    objs = struct.unpack("<%dQ" % count, read(u64(wh, 0x18), 8 * count))

    meshes = []
    for idx, obj in enumerate(objs):
        ob = read(obj, 0xD8)
        shape = read(u64(ob, 0xD0), 0x58)
        if i32(shape, 8) != 21:
            continue
        mi = read(u64(shape, 0x40), 0x38)
        if struct.unpack_from("<3f", mi, 0x10) != (1.0, 1.0, 1.0):
            raise RuntimeError("object %d: mesh scaling is not 1" % idx)
        if i32(mi, 0x24) != 1:
            raise RuntimeError("object %d: expected exactly one indexed mesh" % idx)
        im = read(u64(mi, 0x30), 0x30)
        ntri, nvert = i32(im, 0), i32(im, 0x14)
        istride, vstride = i32(im, 0x10), i32(im, 0x20)
        if istride != 12 or vstride != 12 or i32(im, 0x24) != 2 or i32(im, 0x28) != 0:
            raise RuntimeError("object %d: unexpected index/vertex layout" % idx)
        indices = read(u64(im, 8), 12 * ntri)
        verts = read(u64(im, 0x18), 12 * nvert)
        bvh = read(u64(shape, 0x48), 0x100)
        if bvh[0x48] != 1 or i32(bvh, 0xD0) != 0:
            raise RuntimeError("object %d: bvh is not quantized/stackless" % idx)
        nnodes = i32(bvh, 0x44)
        nodes = read(u64(bvh, 0xC0), 16 * nnodes)
        info = bytearray(struct.pack("<i3f", -1, 0.0, 0.0, 0.0) * ntri)
        tim_ptr = u64(shape, 0x50)
        if tim_ptr:
            tim = read(tim_ptr, 0xB0)
            ninfo = i32(tim, 0x4C)
            if ninfo:
                vals = read(u64(tim, 0x58), 16 * ninfo)
                keys = struct.unpack("<%di" % ninfo, read(u64(tim, 0x78), 4 * ninfo))
                for k, key in enumerate(keys):
                    if key >> 21 != 0 or not 0 <= key < ntri:
                        raise RuntimeError("object %d: unexpected info key %x" % (idx, key))
                    info[16 * key:16 * key + 16] = vals[16 * k:16 * k + 16]
            # convexEpsilon (+0x88), edgeDistanceThreshold (+0x94), maxEdgeAngleThreshold (+0x98)
            # are constants in the sim (arena_mesh.cpp); stop if the game ever changes them.
            params = (tim[0x88:0x8C] + tim[0x94:0x9C]).hex()
            if params != "00000000" "cdcccc3d" "db0fc940":
                raise RuntimeError("object %d: info map thresholds changed: %s" % (idx, params))
        xf = struct.unpack_from("<16f", ob, 0x10)
        blob = struct.pack("<12f", xf[0], xf[1], xf[2], xf[4], xf[5], xf[6], xf[8], xf[9], xf[10],
                           xf[12], xf[13], xf[14])
        blob += bvh[0x10:0x1C] + bvh[0x20:0x2C] + bvh[0x30:0x3C]
        blob += struct.pack("<3I", ntri, nvert, nnodes)
        blob += indices + verts + nodes + bytes(info)
        meshes.append(blob)
        print("object %2d: %4d triangles, %4d vertices, %4d nodes" % (idx, ntri, nvert, nnodes))

    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "wb") as f:
        f.write(b"RLCOL1\0\0" + struct.pack("<I", len(meshes)))
        for m in meshes:
            f.write(m)
    print("wrote %s (%d meshes)" % (out, len(meshes)))


if __name__ == "__main__":
    main()
