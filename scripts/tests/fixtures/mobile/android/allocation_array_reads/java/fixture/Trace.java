package fixture;
public class Trace {
    public static long events;
    public static int fail;
    public static boolean expectLock;
    public static Object lock, seen, reference, input;
    public static Provider provider;
    public static Target published;
    public static Throwable last;
    public static RuntimeException failure;
    public static double wide;
    public static float narrow;
    public static long number;
    public static int integer;
    public static boolean flag;
    public static byte small;
    public static char character;
    public static short medium;
    public static void hit(int event) {
        if (Thread.holdsLock(lock) != expectLock) throw new AssertionError("owned array read lock");
        events = events * 11 + event;
        if (event == fail) throw failure;
    }
}
