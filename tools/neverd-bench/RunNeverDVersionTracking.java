// NeverD-authored adapter invoking Ghidra's Auto Version Tracking engine.
// Profile: bundled AutoVersionTrackingScript.java defaults in Ghidra 12.1.3.
//@category Version Tracking
import com.google.gson.GsonBuilder;
import ghidra.app.script.GhidraScript;
import ghidra.feature.vt.api.db.VTSessionDB;
import ghidra.feature.vt.api.util.VTOptions;
import ghidra.feature.vt.gui.actions.AutoVersionTrackingTask;
import ghidra.feature.vt.gui.util.VTOptionDefines;
import ghidra.framework.Application;
import ghidra.framework.model.DomainFolder;
import ghidra.program.model.listing.Program;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.LinkedHashMap;
import java.util.Map;

public class RunNeverDVersionTracking extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 3 || currentProgram == null || !currentProgram.canSave()) {
            throw new IllegalArgumentException("Expected source path, new session name and receipt");
        }
        DomainFolder root = state.getProject().getProjectData().getRootFolder();
        if (root.getFile(args[1]) != null || Files.exists(Path.of(args[2]))) {
            throw new IllegalArgumentException("Session and receipt must be new");
        }
        Program source = (Program) state.getProject().getProjectData().getFile(args[0])
            .getDomainObject(this, false, false, monitor);
        VTSessionDB session = null;
        try {
            end(true);
            session = new VTSessionDB(args[1], source, currentProgram, this);
            root.createFile(args[1], session, monitor);
            VTOptions options = new VTOptions("NeverD diagnostic Auto VT");
            // The bundled headless script overrides these task defaults.
            options.setInt(VTOptionDefines.DUPE_FUNCTION_CORRELATOR_MIN_LEN_OPTION, 10);
            options.setBoolean(VTOptionDefines.CREATE_IMPLIED_MATCHES_OPTION, true);
            options.setInt(VTOptionDefines.MAX_CONFLICTS_OPTION, 0);
            AutoVersionTrackingTask task = new AutoVersionTrackingTask(session, options);
            task.run(monitor);
            monitor.checkCancelled();
            String status = task.getStatusMsg();
            if (status == null || !status.startsWith("Auto Version Tracking Command completed successfully")) {
                throw new IllegalStateException("Auto Version Tracking did not complete: " + status);
            }
            currentProgram.save("NeverD diagnostic Auto VT", monitor);
            session.save();
            Map<String, Object> result = new LinkedHashMap<>();
            result.put("schema", "neverd.benchmark.ghidra-task.v1");
            result.put("complete", true);
            result.put("ghidra_version", Application.getApplicationVersion());
            result.put("input_sha256", new String[] { source.getExecutableSHA256(),
                currentProgram.getExecutableSHA256() });
            result.put("profile", "AutoVersionTrackingScript-12.1.3-defaults");
            result.put("status", status);
            result.put("session", args[1]);
            monitor.checkCancelled();
            Files.writeString(Path.of(args[2]),
                new GsonBuilder().setPrettyPrinting().create().toJson(result),
                StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE);
        } finally {
            if (session != null) {
                session.release(this);
            }
            source.release(this);
        }
    }
}
