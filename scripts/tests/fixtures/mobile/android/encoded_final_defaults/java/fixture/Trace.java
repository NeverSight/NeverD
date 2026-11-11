package fixture;
public class Trace {
    public static boolean z;
    public static byte b;
    public static char c;
    public static short s;
    public static int i, events, failAt, bad, observations;
    public static long j;
    public static float f;
    public static double d;
    public static Object seed;
    public static Object[] array;
    public static boolean first, second;
    public static RuntimeException failure;
    public static void hit(int step) {
        events = events * 10 + step;
        if (step == failAt) throw failure;
    }
}
