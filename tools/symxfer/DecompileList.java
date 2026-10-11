// Decompile functions by name substring or address and write the C to a file.
// args: <outfile> <pattern|0xADDR> [pattern...]
// A pattern matches when it is a substring of the function's (mangled) name.
// @category Analysis
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;

public class DecompileList extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        List<Function> fs = new ArrayList<>();
        for (int i = 1; i < args.length; i++) {
            String p = args[i];
            if (p.startsWith("0x")) {
                Function f = getFunctionContaining(toAddr(Long.parseLong(p.substring(2), 16)));
                if (f != null) fs.add(f);
                continue;
            }
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                if (f.getName().contains(p)) fs.add(f);
            }
        }
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        try (PrintWriter w = new PrintWriter(args[0])) {
            for (Function f : fs) {
                DecompileResults r = d.decompileFunction(f, 120, monitor);
                w.println("// ==== " + f.getName() + " @ " + f.getEntryPoint());
                if (r != null && r.getDecompiledFunction() != null) {
                    w.println(r.getDecompiledFunction().getC());
                } else {
                    w.println("// decompile failed: " + (r == null ? "null" : r.getErrorMessage()));
                }
            }
        }
        println("decompiled " + fs.size());
    }
}
