#!/usr/bin/env python3
"""Find the AES-256 key(s) Rocket League uses for package headers, straight from the game binary.

Known plaintext: the name table of a package starts with an FString (int32 length, then that many ASCII chars).
We parse the package summary up to NameOffset, then try every 32-byte window (4-byte aligned, high entropy) in the
exe's data sections as an AES-256-ECB key on the 16-byte block at the start of the encrypted region.
Usage: find_upk_key.py <RocketLeague.exe> <package.upk>
"""
import struct, sys
import pefile
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes


def summary(path):
    d = open(path, "rb").read(4096)
    o = 0
    tag, fv, lv, total = struct.unpack_from("<IHHi", d, o); o += 12
    n = struct.unpack_from("<i", d, o)[0]; o += 4 + (n if n >= 0 else -2 * n)   # FolderName
    flags, ncount, noff = struct.unpack_from("<Iii", d, o)
    return dict(tag=tag, ver=(fv, lv), total=total, flags=flags, name_count=ncount, name_offset=noff)


def plausible(block):
    n = struct.unpack_from("<i", block, 0)[0]
    if not (2 <= n <= 64):
        return False
    s = block[4:4 + min(n - 1, 12)]
    return all(48 <= c < 123 or c == 95 for c in s)


def main():
    exe, pkg = sys.argv[1], sys.argv[2]
    s = summary(pkg)
    print("summary", s)
    with open(pkg, "rb") as f:
        f.seek(s["name_offset"]); ct = f.read(16)
    if plausible(ct):
        print("name table is NOT encrypted at NameOffset"); return
    pe = pefile.PE(exe, fast_load=True)
    found = set()
    for sec in pe.sections:
        name = sec.Name.rstrip(b"\0").decode()
        if name not in (".rdata", ".data"):
            continue
        d = sec.get_data()
        for off in range(0, len(d) - 32, 4):
            k = d[off:off + 32]
            if len(set(k)) < 20:          # keys are random bytes
                continue
            pt = Cipher(algorithms.AES(k), modes.ECB()).decryptor().update(ct)
            if plausible(pt):
                rva = sec.VirtualAddress + off
                if k not in found:
                    found.add(k)
                    print(f"KEY rva={rva:x} key={k.hex()} plaintext={pt!r}")
    print(f"{len(found)} key(s) found")


if __name__ == "__main__":
    main()
