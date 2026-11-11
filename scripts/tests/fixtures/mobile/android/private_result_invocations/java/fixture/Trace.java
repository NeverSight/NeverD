package fixture;
public class Trace {
    public static int events, failAt;
    public static RuntimeException failure;
    public static Owner.Worker receiver;
    public static Owner owner;
    public static String label;
    public static Class<?>[] kinds, providerKinds;
    public static Object[] values, providerValues;
    public static long wide;
    public static double real;
    public static void hit(int event) {
        events = events * 10 + event;
        if (event == failAt) throw failure;
    }
}
