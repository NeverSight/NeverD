package fixture;
public class Trace {
    public static long events;
    public static int fail;
    public static boolean expectLock;
    public static Object lock, token;
    public static RuntimeException failure;
    public static Throwable lastFailure;
    public static void hit(int event) {
        if (Thread.holdsLock(lock) != expectLock) throw new AssertionError("owned scalar carrier lock");
        events = events * 11 + event;
        if (event == fail) throw failure;
    }
}
