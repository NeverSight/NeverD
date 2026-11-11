package fixture;
public class Trace {
    public static int events, integer;
    public static long bits, wide;
    public static boolean flag;
    public static double ratio;
    public static Object receiver, context, value;
    public static Bound input;
    public static Runnable action;
    public static void hit(int event) { events = events * 10 + event; }
}
