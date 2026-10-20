// Export every function Ghidra found to JSON, for the recompiler and recon tools.
//
// Usage (headless post-script): ExportFunctions.java <output.json>
// Addresses are written as offsets from the program's image base, so they
// match offsets into work/exefs/<module>.bin.
//
// Output: {"image_base": "0x...", "functions": [
//   {"entry": 1234, "size": 56, "name": "FUN_...", "thunk": false,
//    "ranges": [[start, end_exclusive], ...]}, ...]}
// "ranges" is only present when a function body is not one contiguous block.
//@category YW4

import java.io.BufferedWriter;
import java.io.FileWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

public class ExportFunctions extends GhidraScript {

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 1) {
            printerr("usage: ExportFunctions.java <output.json>");
            return;
        }
        Address base = currentProgram.getImageBase();
        int count = 0;

        try (BufferedWriter out = new BufferedWriter(new FileWriter(args[0]))) {
            out.write("{\"image_base\": \"0x" + Long.toHexString(base.getOffset()) + "\",\n");
            out.write(" \"program\": \"" + escape(currentProgram.getName()) + "\",\n");
            out.write(" \"functions\": [\n");

            FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
            boolean first = true;
            while (it.hasNext() && !monitor.isCancelled()) {
                Function f = it.next();
                if (f.isExternal()) {
                    continue;
                }
                AddressSetView body = f.getBody();
                long entry = f.getEntryPoint().subtract(base);

                StringBuilder sb = new StringBuilder();
                sb.append(first ? "  " : ",\n  ");
                sb.append("{\"entry\": ").append(entry);
                sb.append(", \"size\": ").append(body.getNumAddresses());
                sb.append(", \"name\": \"").append(escape(f.getName(true))).append('"');
                sb.append(", \"thunk\": ").append(f.isThunk());
                if (body.getNumAddressRanges() > 1) {
                    sb.append(", \"ranges\": [");
                    boolean firstRange = true;
                    for (AddressRange r : body.getAddressRanges()) {
                        sb.append(firstRange ? "" : ", ");
                        sb.append('[').append(r.getMinAddress().subtract(base));
                        sb.append(", ").append(r.getMaxAddress().subtract(base) + 1).append(']');
                        firstRange = false;
                    }
                    sb.append(']');
                }
                sb.append('}');
                out.write(sb.toString());
                first = false;
                count++;
            }
            out.write("\n ]}\n");
        }
        println("ExportFunctions: wrote " + count + " functions to " + args[0]);
    }

    private static String escape(String s) {
        StringBuilder sb = new StringBuilder();
        for (char c : s.toCharArray()) {
            if (c == '"' || c == '\\') {
                sb.append('\\').append(c);
            } else if (c < 0x20) {
                sb.append(String.format("\\u%04x", (int) c));
            } else {
                sb.append(c);
            }
        }
        return sb.toString();
    }
}
