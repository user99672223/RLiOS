#!/usr/bin/env python3
"""Verify the offscreen capture PNG: size 2560x1440, exact clear color in the corners, and some
non-clear pixels (the sim point and bounds outline were actually drawn). Pure stdlib."""
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

    for (x, y) in [(2, 2), (w - 3, 2), (2, h - 3), (w - 3, h - 3), (w // 2, 3)]:
        got = px(x, y)
        print(f"pixel({x},{y}) = {got}")
        assert all(abs(g - c) <= 1 for g, c in zip(got, CLEAR)), f"corner not clear color: {got}"
    non_clear = sum(
        1 for y in range(0, h, 4) for x in range(0, w, 4)
        if any(abs(g - c) > 8 for g, c in zip(px(x, y), CLEAR)))
    print(f"non-clear samples (every 4th px): {non_clear}")
    assert non_clear > 200, "nothing drawn on top of the clear color"
    if len(sys.argv) > 2:
        d = json.load(open(sys.argv[2]))
        print("diagnostics ok:", d.get("ok"), "tick:", d.get("sim_tick"), "hash:", d.get("state_hash"))
        assert d.get("ok") is True and d.get("exit_code") == 0
    print("capture verification PASSED")


main()
