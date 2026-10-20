// Create functions at known start offsets, then let auto-analysis follow up.
//
// Usage (headless post-script): SeedFunctions.java <offsets.txt>
// offsets.txt: one hex offset from the image base per line (e.g. 0x1234).
//
// Why: Ghidra's auto-analysis mostly finds functions through direct calls.
// Functions only reached through vtables or callback tables are missed. We
// already know every function start from .eh_frame_hdr (see docs/recon.md),
// so we hand that list to Ghidra here.
//@category YW4

import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Listing;

public class SeedFunctions extends GhidraScript {

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 1) {
            printerr("usage: SeedFunctions.java <offsets.txt>");
            return;
        }
        List<String> lines = Files.readAllLines(Paths.get(args[0]));
        Address base = currentProgram.getImageBase();
        Listing listing = currentProgram.getListing();
        int created = 0, existing = 0, failed = 0;

        monitor.initialize(lines.size());
        for (String line : lines) {
            if (monitor.isCancelled()) {
                break;
            }
            monitor.incrementProgress(1);
            line = line.trim();
            if (line.isEmpty()) {
                continue;
            }
            Address addr = base.add(Long.decode(line));
            if (listing.getFunctionAt(addr) != null) {
                existing++;
                continue;
            }
            if (listing.getInstructionAt(addr) == null) {
                disassemble(addr);
            }
            if (createFunction(addr, null) != null) {
                created++;
            } else {
                failed++;
            }
        }
        println("SeedFunctions: created " + created + ", already existed " + existing
                + ", failed " + failed);
        analyzeChanges(currentProgram);
    }
}
