// Apply a symxfer map ("addr name confidence" per line, '#' comments) to the
// current program: creates a function at each address (if none) and gives it
// the (mangled) name; confidence goes into the plate comment for non-native
// names.  Also accepts plain "addr name" lines (e.g. nm output reordered).
//
// Headless (on your own clone of the project: cp -c -R ~/sims2.{gpr,rep} dir/):
//   analyzeHeadless dir sims2/mac -process sims2_bv.ppc -noanalysis \
//     -scriptPath tools/symxfer -postScript ApplySymxferMap.java map.txt [reset]
// "reset": functions named by an earlier map (plate comment "symxfer: ...")
// that the new map does not name go back to their default FUN_ name, so
// names the new matcher dropped as wrong do not linger.  With -readOnly the
// names live only in that session: chain -postScript DecompileList.java
// after it to decompile with the new names without touching the project.
// @category Symbol
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.SymbolTable;
import java.io.BufferedReader;
import java.io.FileReader;

public class ApplySymxferMap extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String path = args.length > 0 ? args[0] : askFile("symxfer map", "Apply").getPath();
        boolean makeFuncs = true, reset = false;
        for (int i = 1; i < args.length; i++) {
            if (args[i].equals("labels-only")) makeFuncs = false;
            if (args[i].equals("reset")) reset = true;
        }
        java.util.HashSet<Address> named = new java.util.HashSet<>();
        SymbolTable st = currentProgram.getSymbolTable();
        int n = 0, made = 0, bad = 0;
        try (BufferedReader r = new BufferedReader(new FileReader(path))) {
            String line;
            while ((line = r.readLine()) != null) {
                if (line.isEmpty() || line.startsWith("#")) continue;
                String[] p = line.trim().split("\\s+");
                if (p.length < 2) continue;
                Address a = toAddr(Long.parseLong(p[0], 16));
                named.add(a);
                String name = p[1];
                String conf = p.length > 2 ? p[2] : "native";
                if (monitor.isCancelled()) break;
                try {
                    Function f = getFunctionAt(a);
                    if (f == null && makeFuncs && currentProgram.getMemory().getBlock(a) != null
                            && currentProgram.getMemory().getBlock(a).isExecute()) {
                        disassemble(a);
                        CreateFunctionCmd cmd = new CreateFunctionCmd(a);
                        if (cmd.applyTo(currentProgram, monitor)) made++;
                        f = getFunctionAt(a);
                    }
                    if (f != null) {
                        f.setName(name, SourceType.IMPORTED);
                    } else {
                        st.createLabel(a, name, SourceType.IMPORTED);
                    }
                    if (!conf.equals("native")) {
                        setPlateComment(a, "symxfer: " + conf);
                    } else if (reset && getPlateComment(a) != null && getPlateComment(a).startsWith("symxfer:")) {
                        setPlateComment(a, null);
                    }
                    n++;
                } catch (Exception e) {
                    String m = e.getMessage();
                    if (m != null && m.contains("already exists at this address")) { n++; continue; }
                    if (bad < 5) println("symxfer: error at " + a + " " + name + ": " + e.getMessage());
                    bad++;
                }
                if (n % 5000 == 0) println("applied " + n);
            }
        }
        int cleared = 0;
        if (reset) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                Address a = f.getEntryPoint();
                String pc = getPlateComment(a);
                if (named.contains(a) || pc == null || !pc.startsWith("symxfer:")) continue;
                try {
                    f.setName(null, SourceType.DEFAULT);
                    setPlateComment(a, null);
                    cleared++;
                } catch (Exception e) {
                    bad++;
                }
            }
        }
        println("symxfer: cleared " + cleared + " stale names");
        println("symxfer: applied " + n + " names, created " + made + " functions, " + bad + " errors");
    }
}
