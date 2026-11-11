package fixture;

public class Trace {
    public static long events, number;
    public static int failAt;
    public static boolean expectLock;
    public static Object lock, seen, initialValue;
    public static RuntimeException failure;
    public static Throwable last;
    public static Core core;
    public static Target published;

    public static void hit(int event) {
        if (Thread.holdsLock(lock) != expectLock) throw new AssertionError("owned private call monitor ownership");
        events = events * 10 + event;
        if (event == failAt) throw failure;
    }

    public static void consume(Object input) {
        seen = input;
        ((int[]) input)[0]++;
    }
}
