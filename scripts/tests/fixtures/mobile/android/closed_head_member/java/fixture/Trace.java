package fixture;
import java.io.IOException;
import java.util.Map;
public final class Trace {
    private Trace() {}
    public static Object receiver, key;
    public static Class<?> token;
    public static Map result;
    public static Action action;
    public static int events, failAt, calls, actions, gets, handled, overrides;
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
