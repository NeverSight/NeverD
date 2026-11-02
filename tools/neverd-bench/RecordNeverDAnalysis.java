// NeverD-authored completion adapter for Ghidra headless input analysis.
//@category Version Tracking
import com.google.gson.GsonBuilder;
import ghidra.app.util.headless.HeadlessScript;
import ghidra.framework.Application;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.LinkedHashMap;
import java.util.Map;

public class RecordNeverDAnalysis extends HeadlessScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 1 || currentProgram == null) {
            throw new IllegalArgumentException("Expected a program and new receipt path");
        }
        monitor.checkCancelled();
        String identity = currentProgram.getExecutableSHA256();
        if (identity == null || !identity.matches("[0-9a-fA-F]{64}")) {
            throw new IllegalStateException("Program input identity is unavailable");
        }
        Map<String, Object> result = new LinkedHashMap<>();
        result.put("schema", "neverd.benchmark.ghidra-analysis.v1");
        result.put("input_sha256", identity.toLowerCase(java.util.Locale.ROOT));
        result.put("ghidra_version", Application.getApplicationVersion());
        result.put("program", currentProgram.getName());
        result.put("analysis_timed_out", analysisTimeoutOccurred());
        result.put("complete", !analysisTimeoutOccurred());
        Files.writeString(Path.of(args[0]),
            new GsonBuilder().setPrettyPrinting().create().toJson(result),
            StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE);
    }
}
