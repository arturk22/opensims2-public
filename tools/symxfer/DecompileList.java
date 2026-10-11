// Decompile functions by name substring or address and write the C to a file.
// args: <outfile> <pattern|0xADDR> [pattern...]
// Addresses without a function get one (run without -readOnly to keep it).
// A pattern matches a substring of the function name, qualified (cTSObject::TryIdle,
// after auto-analysis demangled the names) or as stored (mangled before analysis).
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
                ghidra.program.model.address.Address a = toAddr(Long.parseLong(p.substring(2), 16));
                Function f = getFunctionContaining(a);
                if (f == null) {   // not a known function yet: create it
                    disassemble(a);
                    f = createFunction(a, null);
                }
                if (f != null) fs.add(f);
                continue;
            }
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                if (f.getName(true).contains(p) || f.getName().contains(p)) fs.add(f);
            }
        }
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        try (PrintWriter w = new PrintWriter(args[0])) {
            for (Function f : fs) {
                DecompileResults r = d.decompileFunction(f, 120, monitor);
                w.println("// ==== " + f.getName(true) + " @ " + f.getEntryPoint());
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
