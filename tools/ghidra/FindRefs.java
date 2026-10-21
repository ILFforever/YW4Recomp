// List the code of main that refers to given addresses (a vtable, a global...), to find e.g.
// a class's constructors from its vtable.
//
// Usage (headless post-script): FindRefs.java <output.txt> <hex offset> [<hex offset> ...]
// Offsets are from the image base. Output: "main+0x<target> main+0x<from> in main+0x<function>"
// per reference. Derived from the game's code: write it under <work_dir>/ (gitignored).
//@category YW4

import java.io.BufferedWriter;
import java.io.FileWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class FindRefs extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            printerr("usage: FindRefs.java <output.txt> <hex offset>...");
            return;
        }
        Address base = currentProgram.getImageBase();
        try (BufferedWriter out = new BufferedWriter(new FileWriter(args[0]))) {
            for (int i = 1; i < args.length; i++) {
                Address target = base.add(Long.parseLong(args[i].replaceFirst("^0x", ""), 16));
                for (Reference r : getReferencesTo(target)) {
                    Address from = r.getFromAddress();
                    Function f = getFunctionContaining(from);
                    out.write(String.format("main+0x%x main+0x%x in main+0x%x  %s\n", target.subtract(base),
                            from.subtract(base), f == null ? 0 : f.getEntryPoint().subtract(base),
                            r.getReferenceType()));
                }
            }
        }
    }
}
