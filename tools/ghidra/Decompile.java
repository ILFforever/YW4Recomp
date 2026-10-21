// Decompile functions of main to C-like text, for reading the engine.
//
// Usage (headless post-script): Decompile.java <output.c> <hex offset> [<hex offset> ...]
// Offsets are from the image base (as yw4's backtraces print them: main+0x...); each one
// may be anywhere inside a function. The output is derived from the game's code: it goes
// under <work_dir>/ (gitignored) and is never committed (CLAUDE.md rule 1).
//@category YW4

import java.io.BufferedWriter;
import java.io.FileWriter;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class Decompile extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            printerr("usage: Decompile.java <output.c> <hex offset>...");
            return;
        }
        Address base = currentProgram.getImageBase();
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        try (BufferedWriter out = new BufferedWriter(new FileWriter(args[0]))) {
            for (int i = 1; i < args.length; i++) {
                long off = Long.parseLong(args[i].replaceFirst("^0x", ""), 16);
                Address a = base.add(off);
                Function f = getFunctionContaining(a);
                if (f == null) {
                    out.write("// main+0x" + Long.toHexString(off) + ": no function\n\n");
                    continue;
                }
                long entry = f.getEntryPoint().subtract(base);
                out.write("// ===== main+0x" + Long.toHexString(entry) + " (asked: main+0x" + Long.toHexString(off) + ")\n");
                DecompileResults r = d.decompileFunction(f, 120, monitor);
                out.write(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed: " + r.getErrorMessage());
                out.write("\n\n");
            }
        }
        d.dispose();
    }
}
