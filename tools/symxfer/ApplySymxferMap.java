// Apply a symxfer map ("addr name confidence" per line, '#' comments) to the
// current program: creates a function at each address (if none) and gives it
// the (mangled) name; confidence goes into the plate comment for non-native
// names.  Also accepts plain "addr name" lines (e.g. nm output reordered).
//
// Headless:
//   analyzeHeadless ~/ sims2 -process sims2_bv.ppc -noanalysis \
//     -scriptPath tools/symxfer -postScript ApplySymxferMap.java map.txt
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
        boolean makeFuncs = args.length < 2 || !args[1].equals("labels-only");
        SymbolTable st = currentProgram.getSymbolTable();
        int n = 0, made = 0, bad = 0;
        try (BufferedReader r = new BufferedReader(new FileReader(path))) {
            String line;
            while ((line = r.readLine()) != null) {
                if (line.isEmpty() || line.startsWith("#")) continue;
                String[] p = line.trim().split("\\s+");
                if (p.length < 2) continue;
                Address a = toAddr(Long.parseLong(p[0], 16));
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
                    }
                    n++;
                } catch (Exception e) {
                    bad++;
                }
                if (n % 5000 == 0) println("applied " + n);
            }
        }
        println("symxfer: applied " + n + " names, created " + made + " functions, " + bad + " errors");
    }
}
