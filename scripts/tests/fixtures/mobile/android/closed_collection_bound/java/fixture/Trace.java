package fixture;
import java.util.Collection;
public class Trace {
    public static String events;
    public static int fail, mode;
    public static RuntimeException failure;
    public static Error error;
    public static CharSequence text;
    public static Collection destination, returned;
    public static Object after;
    public static Character box;
    public static void hit(int event) {
        events = events + event + ",";
        if (fail == event) throw failure;
        if (fail == -event) throw error;
    }
}
