// List the functions of main whose instructions use given constants (e.g. a struct field
// offset), to find the code that reads or writes a field.
//
// Usage (headless post-script): FindScalar.java <output.txt> <hex value> [<hex value> ...]
// Output: "<hex value> main+0x<instruction> in main+0x<function entry>" per use. Large
// offsets are often split by the compiler (add #0x7000 then [#0x148]): search both parts.
// Derived from the game's code: write it under <work_dir>/ (gitignored).
//@category YW4

import java.io.BufferedWriter;
import java.io.FileWriter;
import java.util.HashSet;
import java.util.Set;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.scalar.Scalar;

public class FindScalar extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            printerr("usage: FindScalar.java <output.txt> <hex value>...");
            return;
        }
        Set<Long> want = new HashSet<>();
        for (int i = 1; i < args.length; i++) want.add(Long.parseLong(args[i].replaceFirst("^0x", ""), 16));
        Address base = currentProgram.getImageBase();
        try (BufferedWriter out = new BufferedWriter(new FileWriter(args[0]))) {
            InstructionIterator it = currentProgram.getListing().getInstructions(true);
            while (it.hasNext()) {
                Instruction ins = it.next();
                for (int op = 0; op < ins.getNumOperands(); op++) {
                    for (Object o : ins.getOpObjects(op)) {
                        if (!(o instanceof Scalar)) continue;
                        long v = ((Scalar) o).getUnsignedValue();
                        if (!want.contains(v)) continue;
                        Function f = getFunctionContaining(ins.getAddress());
                        out.write(String.format("%#x main+0x%x in main+0x%x  %s\n", v,
                                ins.getAddress().subtract(base), f == null ? 0 : f.getEntryPoint().subtract(base),
                                ins.toString()));
                    }
                }
            }
        }
    }
}
