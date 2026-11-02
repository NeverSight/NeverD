// Export a completed Version Tracking session without changing associations.
// NeverD-authored benchmark adapter; run inside Ghidra's script environment.
//@category Version Tracking

import com.google.gson.GsonBuilder;
import ghidra.feature.vt.AbstractGhidraVersionTrackingScript;
import ghidra.feature.vt.api.main.VTMatch;
import ghidra.feature.vt.api.main.VTMatchSet;
import ghidra.framework.Application;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public class ExportNeverDVTMatches extends AbstractGhidraVersionTrackingScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 2) {
            throw new IllegalArgumentException("Expected VT session path and new JSON output path");
        }
        // The headless script's current-program transaction would otherwise
        // prevent opening the same destination program through the VT session.
        end(true);
        openVersionTrackingSession(args[0]);
        try {
            String primaryHash = getSourceProgram().getExecutableSHA256();
            String secondaryHash = getDestinationProgram().getExecutableSHA256();
            if (primaryHash == null || !primaryHash.matches("[0-9a-fA-F]{64}") ||
                secondaryHash == null || !secondaryHash.matches("[0-9a-fA-F]{64}")) {
                throw new IllegalArgumentException("VT programs must retain their input SHA-256 identities");
            }
            Map<String, Object> result = new LinkedHashMap<>();
            result.put("schema", "neverd.benchmark.ghidra-vt.v1");
            result.put("ghidra_version", Application.getApplicationVersion());
            result.put("source_sha256", primaryHash.toLowerCase(java.util.Locale.ROOT));
            result.put("destination_sha256", secondaryHash.toLowerCase(java.util.Locale.ROOT));
            result.put("source_functions", getSourceProgram().getFunctionManager().getFunctionCount());
            result.put("destination_functions", getDestinationProgram().getFunctionManager().getFunctionCount());
            result.put("source_image_base", Long.toUnsignedString(getSourceProgram().getImageBase().getOffset()));
            result.put("destination_image_base", Long.toUnsignedString(getDestinationProgram().getImageBase().getOffset()));
            List<String> sourceEntries = new ArrayList<>();
            for (ghidra.program.model.listing.Function function : getSourceProgram().getFunctionManager().getFunctions(true)) {
                monitor.checkCancelled();
                sourceEntries.add(Long.toUnsignedString(function.getEntryPoint().getOffset()));
            }
            List<String> destinationEntries = new ArrayList<>();
            for (ghidra.program.model.listing.Function function : getDestinationProgram().getFunctionManager().getFunctions(true)) {
                monitor.checkCancelled();
                destinationEntries.add(Long.toUnsignedString(function.getEntryPoint().getOffset()));
            }
            result.put("source_function_entries", sourceEntries);
            result.put("destination_function_entries", destinationEntries);
            List<Object> sets = new ArrayList<>();
            for (VTMatchSet set : getVTSession().getMatchSets()) {
                monitor.checkCancelled();
                Map<String, Object> group = new LinkedHashMap<>();
                group.put("correlator", set.getProgramCorrelatorInfo().getName());
                List<Object> rows = new ArrayList<>();
                for (VTMatch match : set.getMatches()) {
                    monitor.checkCancelled();
                    Map<String, Object> row = new LinkedHashMap<>();
                    row.put("source", Long.toUnsignedString(match.getSourceAddress().getOffset()));
                    row.put("destination", Long.toUnsignedString(match.getDestinationAddress().getOffset()));
                    row.put("source_space", match.getSourceAddress().getAddressSpace().getName());
                    row.put("destination_space", match.getDestinationAddress().getAddressSpace().getName());
                    row.put("type", match.getAssociation().getType().toString());
                    row.put("status", match.getAssociation().getStatus().toString());
                    row.put("similarity", match.getSimilarityScore().getScore());
                    row.put("confidence", match.getConfidenceScore().getScore());
                    rows.add(row);
                }
                group.put("matches", rows);
                sets.add(group);
            }
            result.put("match_sets", sets);
            result.put("complete", true);
            monitor.checkCancelled();
            Files.writeString(Path.of(args[1]),
                new GsonBuilder().setPrettyPrinting().create().toJson(result),
                StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE);
        } finally {
            closeVersionTrackingSession();
        }
    }
}
