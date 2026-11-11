// Decompile functions to C text. Headless: -postScript ExportDecomp.java <outdir> <addr-or-name>...
// Each argument is a hex address (image VA or RVA) or a function name; output <outdir>/<name>_<addr>.c
//@category RLiOS
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import java.io.*;
import java.util.*;

public class ExportDecomp extends GhidraScript {
  @Override
  public void run() throws Exception {
    String[] args = getScriptArgs();
    File out = new File(args[0]);
    out.mkdirs();
    DecompInterface ifc = new DecompInterface();
    ifc.openProgram(currentProgram);
    long base = currentProgram.getImageBase().getOffset();
    for (int i = 1; i < args.length; i++) {
      Function f = null;
      try {
        long v = Long.parseLong(args[i], 16);
        Address a = toAddr(v < base ? v + base : v);
        f = getFunctionContaining(a);
        if (f == null) f = createFunction(a, null);
      } catch (NumberFormatException ex) {
        List<Function> l = getGlobalFunctions(args[i]);
        if (!l.isEmpty()) f = l.get(0);
      }
      if (f == null) { println("ExportDecomp: not found: " + args[i]); continue; }
      DecompileResults r = ifc.decompileFunction(f, 180, monitor);
      String c = r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed: " + r.getErrorMessage();
      File of = new File(out, f.getName().replaceAll("[^A-Za-z0-9_]", "_") + "_" + Long.toHexString(f.getEntryPoint().getOffset() - base) + ".c");
      try (PrintWriter w = new PrintWriter(of)) { w.print(c); }
      println("ExportDecomp: " + of.getName());
    }
  }
}
