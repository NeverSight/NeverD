package fixture;
public class Trace {
    public static int events, failAt, comparisons;
    public static long pairs;
    public static boolean expectLock;
    public static Object lock, seen, finished;
    public static RuntimeException failure;
    public static Throwable last;
    public static void hit(int event) {
        if (Thread.holdsLock(lock) != expectLock) throw new AssertionError("owned recursive carrier monitor");
        events = events * 10 + event;
        if (event == failAt) throw failure;
    }
    public static void before(Object values) { hit(1); seen = values; }
    public static void after(Object values) { hit(3); finished = values; }
}
