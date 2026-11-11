package fixture;
public class Trace {
    public static int events, count;
    public static long limit;
    public static double scale;
    public static boolean enabled, inspect, kind;
    public static Object result;
    public static Object[] values;
    public static Class<?> token;
    public static Runnable action;
    public static void hit(int event) { events = events * 10 + event; }
}
