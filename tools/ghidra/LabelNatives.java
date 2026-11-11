// Label UE3 native exec functions from natives.tsv (fn_rva \t table_rva \t name), produced by native_table.py.
// Headless: -postScript LabelNatives.java <natives.tsv>
//@category RLiOS
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;
import java.io.*;
import java.util.*;

public class LabelNatives extends GhidraScript {
  @Override
  public void run() throws Exception {
    String path = getScriptArgs().length > 0 ? getScriptArgs()[0] : askFile("natives.tsv", "Open").getPath();
    long base = currentProgram.getImageBase().getOffset();
    Map<Long, List<String>> byAddr = new TreeMap<>();
    try (BufferedReader r = new BufferedReader(new FileReader(path))) {
      for (String l; (l = r.readLine()) != null;) {
        String[] t = l.split("\t");
        if (t.length < 3) continue;
        byAddr.computeIfAbsent(Long.parseLong(t[0], 16), k -> new ArrayList<>()).add(t[2]);
      }
    }
    int named = 0, shared = 0;
    for (Map.Entry<Long, List<String>> e : byAddr.entrySet()) {
      Address a = toAddr(base + e.getKey());
      Function f = getFunctionAt(a);
      if (f == null) f = createFunction(a, null);
      List<String> names = e.getValue();
      if (names.size() == 1) {
        if (f != null) f.setName(names.get(0), SourceType.IMPORTED);
        else createLabel(a, names.get(0), true, SourceType.IMPORTED);
        named++;
      } else { // one stub shared by several natives: keep every name as a label
        for (String n : names) createLabel(a, n, false, SourceType.IMPORTED);
        shared++;
      }
    }
    println("LabelNatives: named " + named + " functions, " + shared + " shared stubs");
  }
}
