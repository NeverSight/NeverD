package fixture;
public class Trace {
    public static long events;
    public static int failAt;
    public static Object lock = new Object(), input, replacement, result;
    public static String fallback, label;
    public static RuntimeException failure, cleanupFailure;
    public static Throwable last;
    public static boolean held, cleanupAtThree, complete;
    public static void hit(int event) {
        events = events * 10 + event;
        held = Thread.holdsLock(lock);
        if (event == 2) ((Object[]) input)[0] = replacement;
        if (event == 3 && cleanupAtThree) throw cleanupFailure;
        if (event == failAt) { if (event == 5) throw cleanupFailure; throw failure; }
    }
    public static Object read(Object source, String name) {
        input = source; label = name; hit(1);
        name.length();
        Object result = ((Object[]) source)[0];
        hit(2); return result;
    }
}
