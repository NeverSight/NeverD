package fixture;
public class Trace {
    public static int events, offset;
    public static long limit;
    public static double scale;
    public static boolean enabled;
    public static Object context, result;
    public static Object[] values;
    public static Runnable action;
    public static void hit(int event) { events = events * 10 + event; }
}
