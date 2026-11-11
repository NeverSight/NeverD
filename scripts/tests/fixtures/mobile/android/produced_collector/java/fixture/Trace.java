package fixture;
import java.util.Collection;
public final class Trace {
    private Trace() {}
    public static int targets;
    public static long events;
    public static Object supplier, result;
    public static Collection collection;
    public static Runnable action;
    public static void hit(int digit) { events = events * 10 + digit; }
}
