package fixture;
public class Trace {
    public static long events;
    public static int fail;
    public static RuntimeException failure;
    public static Error error;
    public static Provider current, other, seen;
    public static Object after, late;
    public static void hit(int event) {
        events = events * 11 + event;
        if (event == 2) current = other;
        if (event == 3) seen.value = late;
        if (fail == event) throw failure;
        if (fail == -event) throw error;
    }
}
