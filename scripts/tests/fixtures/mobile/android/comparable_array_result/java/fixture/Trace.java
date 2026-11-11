package fixture;
import java.util.Comparator;
public class Trace {
    public static int events, comparison;
    public static boolean invoke;
    public static Object result;
    public static Comparable[] values;
    public static Comparator comparator;
    public static Runnable action;
    public static void hit(int event) { events = events * 10 + event; }
}
