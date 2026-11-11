#!/usr/bin/env python3
"""Index which package files contain which top-level packages (cooked RL packages bundle their dependencies).

Reads only the (decrypted) header of each .upk: names, imports, exports. Writes TSV lines:
    <file>  <top-level package name>  <number of exports under it>
Usage: upk_index.py <keyfile> <CookedPCConsole dir> <out.tsv>
"""
import os, struct, sys
from collections import Counter
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import upk_unpack as u  # noqa: E402


def read_header(path, keys):
    with open(path, "rb") as f:
        d = f.read(65536)
        s = u.parse_summary(d)
        if s["total"] > len(d):
            f.seek(0)
            d = f.read(s["total"])
    o = 12
    o = u.read_fstring(d, o)
    _flags, ncount, noff, ecount, eoff, icount, ioff = struct.unpack_from("<Iiiiiii", d, o)
    if s["chunkinfo"] == 0:
        hdr, base = d, 0
    else:
        L = (s["total"] - s["garbage"] - s["noff"] + 15) // 16 * 16
        ct = d[s["noff"]:s["noff"] + L]
        for k in keys:
            hdr = Cipher(algorithms.AES(k), modes.ECB()).decryptor().update(ct)
            if u.plausible_names(hdr):
                break
        else:
            raise ValueError("no key")
        base = s["noff"]
    names = []
    p = noff - base
    for _ in range(ncount):
        n = struct.unpack_from("<i", hdr, p)[0]
        names.append(hdr[p + 4:p + 4 + n - 1].decode("latin-1")); p += 4 + n + 8  # + object flags (u64)
    imports = []
    p = ioff - base
    for _ in range(icount):
        _cp, _cpn, cls, _cn, outer, nm, _nn = struct.unpack_from("<iiiiiii", hdr, p); p += 28
        imports.append((names[cls], outer, names[nm]))
    exports = []
    p = eoff - base
    for _ in range(ecount):
        cls, _sup, outer, nm, _nn, _arch = struct.unpack_from("<iiiiii", hdr, p); p += 24
        p += 8 + 4 + 8 + 4  # ObjectFlags u64, SerialSize, SerialOffset (64-bit, RL >= 22), ExportFlags
        k = struct.unpack_from("<i", hdr, p)[0]; p += 4 + 4 * k  # GenerationNetObjectCount
        p += 16 + 4  # PackageGuid, PackageFlags
        exports.append((cls, outer, names[nm]))
    return names, imports, exports


def top_packages(exports):
    def root(i):
        seen = 0
        while exports[i][1] > 0 and seen < 64:
            i = exports[i][1] - 1; seen += 1
        return exports[i][2]
    return Counter(root(i) for i in range(len(exports)) if exports[i][1] > 0)


def main():
    keys = [bytes.fromhex(l.strip()) for l in open(sys.argv[1]) if l.strip()]
    d = sys.argv[2]
    bad = 0
    with open(sys.argv[3], "w") as out:
        for fn in sorted(os.listdir(d)):
            if not fn.endswith(".upk"):
                continue
            try:
                _n, _i, ex = read_header(os.path.join(d, fn), keys)
            except Exception as e:  # noqa: BLE001 (report and continue)
                bad += 1
                print(fn, "FAILED", e, file=sys.stderr)
                continue
            for pkg, cnt in top_packages(ex).items():
                out.write(f"{fn}\t{pkg}\t{cnt}\n")
    print("failed:", bad, file=sys.stderr)


if __name__ == "__main__":
    main()
