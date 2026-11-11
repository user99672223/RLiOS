#!/usr/bin/env python3
"""Quick linear disassembly of a function by RVA: dis.py exe rva [maxbytes]. Stops at first ret/int3 run after no pending forward branch."""
import sys, pefile, capstone, struct
pe = pefile.PE(sys.argv[1], fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase
rva = int(sys.argv[2], 16); n = int(sys.argv[3]) if len(sys.argv) > 3 else 400
data = pe.get_data(rva, n)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = False
maxt = 0
for i in md.disasm(data, base + rva):
    extra = ''
    if 'rip' in i.op_str:
        import re
        m = re.search(r'rip ([+-]) (0x[0-9a-f]+)', i.op_str)
        if m:
            t = i.address + i.size + (int(m.group(2), 16) * (1 if m.group(1) == '+' else -1))
            extra = f'   ; -> {t:x}'
            try:
                raw = pe.get_data(t - base, 8)
                extra += f' [{struct.unpack("<f", raw[:4])[0]:.9g}f / {struct.unpack("<d", raw)[0]:.9g} / {struct.unpack("<Q", raw)[0]:x}]'
            except Exception: pass
    print(f'{i.address:x}: {i.mnemonic} {i.op_str}{extra}')
    if i.mnemonic.startswith('j') and i.op_str.startswith('0x'):
        maxt = max(maxt, int(i.op_str, 16))
    if i.mnemonic in ('ret', 'int3') or (i.mnemonic == 'jmp' ) :
        if i.address >= maxt: break
