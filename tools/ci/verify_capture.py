#!/usr/bin/env python3
"""Verify the offscreen capture PNG: size 2560x1440, a rendered scene on top of the clear/sky colour (not
uniform), diagnostics ok; with --expect-pack also that the real asset pack was loaded. Pure stdlib.
Usage: verify_capture.py <capture.png> [diagnostics.json [--expect-pack]]"""
import json
import struct
import sys
import zlib

EXPECT_W, EXPECT_H = 2560, 1440
CLEAR = (51, 102, 204)


def read_png(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos, idat, ihdr = 8, b"", None
    while pos < len(data):
        n, typ = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if typ == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif typ == b"IDAT":
            idat += body
        pos += 12 + n
    w, h, depth, ctype, _, _, interlace = ihdr
    assert depth == 8 and interlace == 0 and ctype in (2, 6), f"unsupported PNG {ihdr}"
    bpp = 3 if ctype == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    p = 0
    for _ in range(h):
        f = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        rows.append(line)
        prev = line
    return w, h, bpp, rows


def main():
    w, h, bpp, rows = read_png(sys.argv[1])
    print(f"size {w}x{h}")
    assert (w, h) == (EXPECT_W, EXPECT_H), "wrong size"

    def px(x, y):
        return tuple(rows[y][x * bpp:x * bpp + 3])

    samples = [px(x, y) for y in range(0, h, 8) for x in range(0, w, 8)]
    sky = sum(1 for p in samples if all(abs(g - c) <= 2 for g, c in zip(p, CLEAR)))
    distinct = len(set(samples))
    print(f"samples {len(samples)}: sky/clear {sky}, distinct colours {distinct}")
    assert sky < len(samples) * 0.9, "almost nothing drawn over the clear colour"
    assert distinct > 200, "image too uniform to be a rendered scene"
    if len(sys.argv) > 2:
        d = json.load(open(sys.argv[2]))
        scene = d.get("scene", {})
        print("diagnostics ok:", d.get("ok"), "tick:", d.get("sim_tick"), "scene:", scene)
        assert d.get("ok") is True and d.get("exit_code") == 0
        if len(sys.argv) > 3 and sys.argv[3] == "--expect-pack":
            assert scene.get("real_pack") is True, "asset pack was provided but not loaded"
            assert scene.get("instances", 0) > 400, "too few instances from the pack"
            assert distinct > 2000, "real arena should be far more detailed"
    print("capture verification PASSED")


main()
