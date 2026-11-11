import com.sun.source.tree.ClassTree;
import com.sun.source.tree.CompilationUnitTree;
import com.sun.source.util.JavacTask;
import com.sun.source.util.Plugin;
import com.sun.source.util.SourcePositions;
import com.sun.source.util.TaskEvent;
import com.sun.source.util.TaskListener;
import com.sun.source.util.TreePath;
import com.sun.source.util.TreePathScanner;
import com.sun.source.util.Trees;
import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardOpenOption;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import javax.lang.model.element.Element;
import javax.lang.model.element.TypeElement;

/** Record javac's own binary names, including local and anonymous declarations. */
public final class RecoveryClassNames implements Plugin {
    private static final Pattern MARKER =
        Pattern.compile("(?m)^[ \\t]*// NeverD class ([0-9a-f]+)[ \\t]*$");

    @Override public String getName() {
        return "NeverDClassNames";
    }

    @Override public void init(JavacTask task, String... args) {
        Trees trees = Trees.instance(task);
        SourcePositions positions = trees.getSourcePositions();
        Map<String, String> classes = new TreeMap<>();
        Path output = Paths.get(System.getProperty("neverd.class.names"));

        task.addTaskListener(new TaskListener() {
            @Override public void started(TaskEvent event) {}

            @Override public void finished(TaskEvent event) {
                try {
                    if (event.getKind() == TaskEvent.Kind.COMPILATION) {
                        List<String> rows = new ArrayList<>();
                        classes.forEach((id, name) -> rows.add(id + "\t" + name));
                        Files.write(output, rows, StandardCharsets.UTF_8, StandardOpenOption.CREATE_NEW);
                        return;
                    }
                    if (event.getKind() != TaskEvent.Kind.ANALYZE) return;
                    CompilationUnitTree unit = event.getCompilationUnit();
                    Matcher matcher = MARKER.matcher(unit.getSourceFile().getCharContent(true));
                    TreeMap<Long, String> markers = new TreeMap<>();
                    while (matcher.find()) markers.put((long) matcher.start(), matcher.group(1));
                    TreePath root = trees.getPath(event.getTypeElement());
                    if (root == null) return; // package-info class
                    new TreePathScanner<Void, Void>() {
                        @Override public Void visitClass(ClassTree node, Void unused) {
                            long start = positions.getStartPosition(unit, node);
                            long end = positions.getEndPosition(unit, node);
                            Map.Entry<Long, String> marker = markers.ceilingEntry(start);
                            if (start >= 0 && marker != null && marker.getKey() < end) {
                                Element element = trees.getElement(getCurrentPath());
                                if (!(element instanceof TypeElement)) {
                                    throw new IllegalStateException("Unbound recovered class at source position " + start);
                                }
                                String name = task.getElements().getBinaryName((TypeElement) element).toString();
                                if (name.isEmpty() || classes.putIfAbsent(marker.getValue(), name) != null) {
                                    throw new IllegalStateException("Duplicate or unnamed recovered class " + marker.getValue());
                                }
                            }
                            return super.visitClass(node, unused);
                        }
                    }.scan(root, null);
                } catch (IOException e) {
                    throw new UncheckedIOException(e);
                }
            }
        });
    }
}
