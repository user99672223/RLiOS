# Reverse-engineering scripts

Scripts only; Ghidra projects and all output live in `work/ghidra/`.

| File | Purpose |
|---|---|
| `native_table.py` | Recovers the UE3 native function table (`<Class>exec<Func>` → address) straight from the PE. |
| `rvadis.py` | Disassembles a function by RVA (capstone), annotating RIP-relative constants. |
| `xcalls.py` | Finds direct call/jmp sites to given RVAs and their containing functions (via `.pdata`). |
| `headless.sh` | Ghidra headless launcher with a 9 GB heap. |
| `LabelNatives.java` | Ghidra post-script: names functions from `natives.tsv`. |
| `ExportDecomp.java` | Ghidra post-script: writes decompiled C for chosen functions. |
