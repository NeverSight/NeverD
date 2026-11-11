package fixture;
import java.io.IOException;
import java.util.Set;
public final class Trace {
    private Trace() {}
    public static Object receiver, value;
    public static String context;
    public static Class<?>[] types;
    public static Set result, collection;
    public static Action action;
    public static int events, failAt, calls, actions, inspected, handled, overrides, collections;
    public static IOException checked, caught;
    public static RuntimeException runtime;
    public static Error error;
    public static void hit(int stage) throws IOException {
        events = events * 10 + stage;
        if (stage == failAt) {
            if (checked != null) throw checked;
            if (error != null) throw error;
            throw runtime;
        }
    }
}
