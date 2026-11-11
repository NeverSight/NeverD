package fixture;
public class Trace {
    public static int events, selector;
    public static Object context, result;
    public static Object[] values;
    public static Runnable action;
    public static void hit(int event) { events = events * 10 + event; }
}
