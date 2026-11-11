#!/usr/bin/env python3
"""Find direct call/jmp rel32 sites targeting given RVAs, and report containing function via .pdata. xcalls.py exe rva..."""
import sys, pefile, struct, bisect
pe = pefile.PE(sys.argv[1], fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase
text = [s for s in pe.sections if s.Name.startswith(b'.text')][0]
d = text.get_data(); tva = text.VirtualAddress
pd = [s for s in pe.sections if s.Name.startswith(b'.pdata')][0].get_data()
funcs = sorted(struct.unpack_from('<III', pd, i)[:2] for i in range(0, len(pd) - 11, 12))
starts = [f[0] for f in funcs]
def fn(rva):
    i = bisect.bisect_right(starts, rva) - 1
    return funcs[i][0] if i >= 0 and funcs[i][1] > rva else 0
targets = {int(a, 16) for a in sys.argv[2:]}
import re
for m in re.finditer(rb'[\xe8\xe9]', d):
    o = m.start()
    if o + 5 > len(d): break
    t = tva + o + 5 + struct.unpack_from('<i', d, o + 1)[0]
    if t in targets:
        print(f"{t:x} <- {'call' if d[o]==0xe8 else 'jmp '} at {base+tva+o:x} in func {fn(tva+o):x}")
