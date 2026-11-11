package fixture;
public class Trace {
    public static long events;
    public static int failAt, seed;
    public static boolean expectLock;
    public static Object lock, seen;
    public static RuntimeException failure;
    public static Throwable last;
    public static Target published;
    public static void hit(int event) {
        if (Thread.holdsLock(lock) != expectLock) throw new AssertionError("owned payload lock");
        events = events * 10 + event;
        if (event == failAt) throw failure;
    }
    public static int before(Object input) {
        hit(1);
        return seed + ((int[]) input)[0]++;
    }
    public static int after(Object input) {
        seen = input;
        hit(2);
        return ((int[]) input)[0]++;
    }
}
