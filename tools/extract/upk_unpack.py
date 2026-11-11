#!/usr/bin/env python3
"""Decrypt and decompress Rocket League .upk packages into plain UE3 packages (for umodel and our own readers).

Format (measured on the 2026-10-31 build, package version 868/34):
  - Summary is plaintext up to NameOffset. After the standard UE3 fields (..., CompressionFlags, NumCompressedChunks=0,
    PackageSource, AdditionalPackagesToCook, TextureAllocations) RL adds: GarbageSize, CompressedChunkInfoOffset,
    LastBlockSize.
  - Bytes [NameOffset, TotalHeaderSize - GarbageSize) rounded up to 16 are AES-256-ECB encrypted. The key is not in
    the exe in the clear; it is captured from the game's AESDecrypt (RVA 0x2DB210) by tools/hook (aes_keys.txt).
  - At CompressedChunkInfoOffset (relative to NameOffset, inside the decrypted block): int32 count, then per chunk
    int64 UncompressedOffset, int32 UncompressedSize, int64 CompressedOffset, int32 CompressedSize, 12 more bytes
    (zero in every package seen so far; meaning unknown), i.e. a 36-byte stride.
  - Each chunk is a standard UE3 compressed chunk (tag 0x9E2A83C1, block size, summary, block table, zlib blocks).
Output: header with decrypted names/imports/exports, followed by the uncompressed body at its uncompressed offsets;
CompressionFlags is set to 0 so readers take it as an uncompressed package.

Usage: upk_unpack.py <keyfile> <out_dir> <package.upk>...    (keyfile: one hex AES-256 key per line; all are tried)
"""
import os, struct, sys, zlib
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

TAG = 0x9E2A83C1


def read_fstring(d, o):
    n = struct.unpack_from("<i", d, o)[0]
    return o + 4 + (n if n >= 0 else -2 * n)


def parse_summary(d):
    o = 4
    _fv, _lv, total = struct.unpack_from("<HHi", d, o); o = 12
    o = read_fstring(d, o)  # FolderName
    flags, ncount, noff = struct.unpack_from("<Iii", d, o)
    o += 48 + 16  # 12 int fields (flags .. ThumbnailTableOffset), Guid
    gc = struct.unpack_from("<i", d, o)[0]; o += 4 + 12 * gc
    o += 8  # EngineVersion, CookerVersion
    cflags_off = o
    cflags, nchunks = struct.unpack_from("<Ii", d, o); o += 8
    o += 4  # PackageSource
    n = struct.unpack_from("<i", d, o)[0]; o += 4
    for _ in range(n):
        o = read_fstring(d, o)
    n = struct.unpack_from("<i", d, o)[0]; o += 4  # TextureAllocations: SizeX, SizeY, NumMips, Format, CreateFlags, TArray<int>
    for _ in range(n):
        o += 20
        k = struct.unpack_from("<i", d, o)[0]; o += 4 + 4 * k
    garbage, chunkinfo, lastblock = struct.unpack_from("<iii", d, o)
    return dict(total=total, noff=noff, cflags=cflags, cflags_off=cflags_off, nchunks=nchunks, garbage=garbage,
                chunkinfo=chunkinfo, lastblock=lastblock)


def plausible_names(pt):
    n = struct.unpack_from("<i", pt, 0)[0]
    return 2 <= n <= 1024 and all(32 <= c < 127 for c in pt[4:4 + min(n - 1, 12)])


def decompress_chunk(d, off):
    tag, bsize, csize, usize = struct.unpack_from("<Iiii", d, off)
    if tag != TAG:
        raise ValueError("bad chunk tag at %d" % off)
    nblocks = (usize + bsize - 1) // bsize
    p = off + 16
    sizes = [struct.unpack_from("<ii", d, p + 8 * i) for i in range(nblocks)]
    p += 8 * nblocks
    out = bytearray()
    for c, u in sizes:
        b = zlib.decompress(d[p:p + c]); p += c
        if len(b) != u:
            raise ValueError("block size mismatch")
        out += b
    return bytes(out)


def unpack(path, keys, out_dir):
    d = open(path, "rb").read()
    s = parse_summary(d)
    if s["cflags"] == 0 and s["nchunks"] == 0 and s["chunkinfo"] == 0:
        return "plain"
    L = (s["total"] - s["garbage"] - s["noff"] + 15) // 16 * 16
    ct = d[s["noff"]:s["noff"] + L]
    for k in keys:
        pt = Cipher(algorithms.AES(k), modes.ECB()).decryptor().update(ct)
        if plausible_names(pt):
            break
    else:
        raise ValueError("no key decrypts the name table")
    ci = s["chunkinfo"]
    n = struct.unpack_from("<i", pt, ci)[0]
    chunks = [struct.unpack_from("<qiqi", pt, ci + 4 + 36 * i) for i in range(n)]  # 36-byte stride (12 RL bytes)
    body_start = min(c[0] for c in chunks) if chunks else s["total"]
    size = max((c[0] + c[1] for c in chunks), default=body_start)
    out = bytearray(size)
    out[:s["noff"]] = d[:s["noff"]]
    out[s["noff"]:body_start] = pt[:body_start - s["noff"]]
    struct.pack_into("<I", out, s["cflags_off"], 0)
    for uoff, usize, coff, csize in chunks:
        b = decompress_chunk(d, coff)
        if len(b) != usize:
            raise ValueError("chunk size mismatch")
        out[uoff:uoff + usize] = b
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, os.path.basename(path)), "wb") as f:
        f.write(out)
    return "%d chunks, %d bytes" % (n, len(out))


def main():
    keys = [bytes.fromhex(l.strip()) for l in open(sys.argv[1]) if l.strip()]
    out_dir = sys.argv[2]
    bad = 0
    for p in sys.argv[3:]:
        try:
            print(os.path.basename(p), unpack(p, keys, out_dir))
        except Exception as e:  # report and continue; one odd package must not stop a full run
            bad += 1
            print(os.path.basename(p), "FAILED", e)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
