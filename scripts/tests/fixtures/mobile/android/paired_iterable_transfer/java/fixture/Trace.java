package fixture;
import java.util.Map;
public class Trace {
    public static String events;
    public static int fail, mode, firstCount;
    public static RuntimeException failure;
    public static Error error;
    public static Map input, sink, output, returned, alternate;
    public static Iterable rows;
    public static Pair[] original;
    public static Pair next;
    public static Object after;
    public static void hit(int event) {
        events = events + event + ",";
        if (fail == event) throw failure;
        if (fail == -event) throw error;
    }
}
