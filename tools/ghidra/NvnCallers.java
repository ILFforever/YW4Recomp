// Trace NVN entry points from their name strings to the functions that call them.
//
// Usage (headless post-script): NvnCallers.java <names.txt> <output.json>
// names.txt: "<hex offset> <nvnName>" per line, offsets from the image base
// (where each name string lives in main).
//
// The NVN loader pattern is: load the address of "nvnFoo", call GetProcAddress,
// store the returned pointer into a slot. Code that uses nvnFoo later loads the
// slot and calls through it. So for each name we:
//   1. find code that references the string (the loader),
//   2. scan forward from there for the first store that writes a memory
//      reference (the slot),
//   3. collect every instruction that reads the slot, and its function.
// Names whose slot can't be resolved this way are reported with "slot": null.
//@category YW4

import java.io.BufferedWriter;
import java.io.FileWriter;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.List;
import java.util.TreeMap;
import java.util.TreeSet;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.symbol.Reference;

public class NvnCallers extends GhidraScript {

    private static final int SCAN_LIMIT = 24;

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 2) {
            printerr("usage: NvnCallers.java <names.txt> <output.json>");
            return;
        }
        Address base = currentProgram.getImageBase();
        Listing listing = currentProgram.getListing();
        List<String> lines = Files.readAllLines(Paths.get(args[0]));
        int resolved = 0;

        try (BufferedWriter out = new BufferedWriter(new FileWriter(args[1]))) {
            out.write("{\"entries\": [\n");
            boolean first = true;
            for (String line : lines) {
                String[] parts = line.trim().split("\\s+");
                if (parts.length != 2) {
                    continue;
                }
                Address str = base.add(Long.decode(parts[0]));
                String name = parts[1];

                TreeSet<Long> loaderSites = new TreeSet<>();
                Address slot = null;
                for (Reference ref : getReferencesTo(str)) {
                    Instruction insn = listing.getInstructionAt(ref.getFromAddress());
                    if (insn == null) {
                        continue;
                    }
                    loaderSites.add(insn.getAddress().subtract(base));
                    if (slot == null) {
                        slot = findStoreTarget(insn);
                    }
                }

                TreeMap<Long, String> readers = new TreeMap<>();
                int readSites = 0;
                if (slot != null) {
                    resolved++;
                    for (Reference ref : getReferencesTo(slot)) {
                        if (!ref.getReferenceType().isRead()) {
                            continue;
                        }
                        readSites++;
                        Function f = getFunctionContaining(ref.getFromAddress());
                        if (f != null) {
                            readers.put(f.getEntryPoint().subtract(base), f.getName(true));
                        }
                    }
                }

                StringBuilder sb = new StringBuilder(first ? "  " : ",\n  ");
                sb.append("{\"name\": \"").append(name).append('"');
                sb.append(", \"loader_sites\": ").append(loaderSites.size());
                sb.append(", \"slot\": ").append(slot == null ? "null" : String.valueOf(slot.subtract(base)));
                sb.append(", \"read_sites\": ").append(readSites);
                sb.append(", \"callers\": [");
                boolean firstCaller = true;
                for (var e : readers.entrySet()) {
                    sb.append(firstCaller ? "" : ", ");
                    sb.append("[").append(e.getKey()).append(", \"").append(escape(e.getValue())).append("\"]");
                    firstCaller = false;
                }
                sb.append("]}");
                out.write(sb.toString());
                first = false;
            }
            out.write("\n]}\n");
        }
        println("NvnCallers: resolved slots for " + resolved + " of " + lines.size() + " names");
    }

    /** First store after `from` (within SCAN_LIMIT instructions) that writes to a known address. */
    private Address findStoreTarget(Instruction from) {
        Instruction insn = from;
        for (int i = 0; i < SCAN_LIMIT && insn != null; i++) {
            if (insn.getMnemonicString().toLowerCase().startsWith("st")) {
                for (Reference r : insn.getReferencesFrom()) {
                    RefType t = r.getReferenceType();
                    if (t.isWrite() && r.getToAddress().isMemoryAddress()) {
                        return r.getToAddress();
                    }
                }
            }
            insn = insn.getNext();
        }
        return null;
    }

    private static String escape(String s) {
        return s.replace("\\", "\\\\").replace("\"", "\\\"");
    }
}
