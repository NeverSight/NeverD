package fixture;
public class DefaultTrace {
    public static int calls, events, overrides;
    public static Object[] values;
    public static Object seen;
    public static long wide;
    public static double real;
    public static RuntimeException failure;
    public static void hit(int value) { events = events * 10 + value; }
}
