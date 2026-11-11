package fixture;

public class Trace {
    public static long events;
    public static int failAt;
    public static boolean expectLock;
    public static Object lock, seen;
    public static RuntimeException failure;
    public static Throwable last;
    public static Provider provider;
    public static Target published;
    public static long number;
    public static double wide;
    public static float narrow;
    public static int integer;
    public static boolean flag;

    public static void hit(int event) {
        if (Thread.holdsLock(lock) != expectLock) throw new AssertionError("owned conversion monitor ownership");
        events = events * 10 + event;
        if (event == failAt) throw failure;
    }

    public static void consume(Object input) {
        seen = input;
        ((int[]) input)[0]++;
    }
}
