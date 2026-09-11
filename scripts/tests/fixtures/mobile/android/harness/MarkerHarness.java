import fixture.*;
import java.io.BufferedWriter;
import java.lang.annotation.Annotation;
import java.lang.annotation.Documented;
import java.lang.annotation.Inherited;
import java.lang.annotation.Retention;
import java.lang.annotation.Target;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

public final class MarkerHarness {
    private static String names(Annotation[] annotations) {
        List<String> names = new ArrayList<>();
        for (Annotation annotation : annotations) {
            names.add(annotation.annotationType().getName());
        }
        Collections.sort(names);
        return "types=" + String.join(",", names);
    }

    private static String definition(Class<?> type) {
        Retention retention = type.getDeclaredAnnotation(Retention.class);
        Target target = type.getDeclaredAnnotation(Target.class);
        String targets = "absent";
        if (target != null) {
            List<String> values = new ArrayList<>();
            for (java.lang.annotation.ElementType value : target.value()) {
                values.add(value.name());
            }
            targets = "[" + String.join(",", values) + "]";
        }
        return "annotation=" + type.isAnnotation() + ";members=" + type.getDeclaredMethods().length
            + ";retention=" + (retention == null ? "absent" : retention.value().name())
            + ";target=" + targets
            + ";documented=" + type.isAnnotationPresent(Documented.class)
            + ";inherited=" + type.isAnnotationPresent(Inherited.class);
    }

    private static void emit(String key, int value) {
        System.out.println(key + "=" + value);
    }

    public static void main(String[] arguments) throws Exception {
        if (arguments.length != 1) {
            throw new IllegalArgumentException("one reflection output path is required");
        }
        Class<?>[] types = {
            MarkerDefault.class, MarkerClass.class, MarkerSource.class, MarkerRuntime.class,
            MarkerInherited.class, MarkerEmptyTarget.class, MarkerTypeOnly.class,
            MarkerAnnotationOnly.class, MarkerTagged.class, MarkerBase.class, MarkerChild.class,
            MarkerPlain.class, MarkerInterface.class, MarkerImplementer.class, MarkerSourceUse.class
        };
        Map<String, String> records = new TreeMap<>();
        for (Class<?> type : types) {
            records.put(type.getSimpleName() + ".declared", names(type.getDeclaredAnnotations()));
            records.put(type.getSimpleName() + ".present", names(type.getAnnotations()));
            if (type.isAnnotation()) {
                records.put(type.getSimpleName() + ".definition", definition(type));
            }
        }
        try (BufferedWriter writer = Files.newBufferedWriter(Paths.get(arguments[0]), StandardCharsets.UTF_8)) {
            for (Map.Entry<String, String> record : records.entrySet()) {
                writer.write(record.getKey() + "\t" + record.getValue() + "\n");
                emit("reflection:" + record.getKey(), 1);
            }
        }
        int[] inputs = {-100, -1, 0, 1, 7, 100};
        MarkerPlain plain = new MarkerPlain();
        MarkerImplementer implementer = new MarkerImplementer();
        MarkerSourceUse sourceUse = new MarkerSourceUse();
        for (int index = 0; index < inputs.length; index++) {
            int value = inputs[index];
            MarkerBase base = new MarkerBase(value);
            MarkerChild child = new MarkerChild(value);
            emit("base-seed:" + index, base.seed);
            emit("base-adjust:" + index, base.adjust(5));
            emit("child-seed:" + index, child.seed);
            emit("child-shifted:" + index, child.shifted(-2));
            emit("plain:" + index, plain.value(value));
            emit("implements:" + index, implementer.value(value));
            emit("source-use:" + index, sourceUse.value(value));
        }
    }
}
