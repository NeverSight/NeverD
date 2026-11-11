package fixture;
public class Trace {
    public static int events, comparison;
    public static boolean read;
    public static Object result;
    public static Comparable[] values;
    public static Runnable action;
    public static void hit(int event) { events = events * 10 + event; }
}
