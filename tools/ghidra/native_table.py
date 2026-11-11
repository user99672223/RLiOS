#!/usr/bin/env python3
"""Recover UE3 native-function registration tables ({const char* name; void* fn} pairs)
from the PE without a disassembler. Output: TSV  fn_rva  name."""
import sys, struct, re, pefile
pe = pefile.PE(sys.argv[1], fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
secs = {s.Name.rstrip(b'\0').decode(): s for s in pe.sections}
def raw(s): return s.get_data()
text = secs['.text']; tlo = base + text.VirtualAddress; thi = tlo + text.Misc_VirtualSize
out = {}
# name strings: ASCII "<Class>exec<Func>\0"
strs = {}
for sn in ('.rdata', '.data'):
    s = secs[sn]; d = raw(s); va = base + s.VirtualAddress
    for m in re.finditer(rb'[A-Za-z_][A-Za-z0-9_]{2,80}exec[A-Za-z0-9_]{1,80}\0', d):
        if m.start() == 0 or d[m.start()-1] == 0:
            strs[va + m.start()] = m.group()[:-1].decode()
for sn in ('.rdata', '.data'):
    s = secs[sn]; d = raw(s); va = base + s.VirtualAddress
    for off in range(0, len(d) - 16, 8):
        p, f = struct.unpack_from('<QQ', d, off)
        if p in strs and tlo <= f < thi:
            out[va + off] = (f, strs[p])
for a in sorted(out):
    f, n = out[a]
    print(f"{f-base:08x}\t{a-base:08x}\t{n}")
sys.stderr.write(f"{len(strs)} exec strings, {len(out)} table entries\n")
