package fixture;
public class Trace {
    public static long events;
    public static int mode, width, height;
    public static Object lock = new Object(), entryLock;
    public static boolean held, capacityHeld, insideHeld, afterHeld;
    public static RuntimeException failure;
    public static void hit(int event) {
        events = events * 10 + event;
        held = entryLock != null && Thread.holdsLock(entryLock);
        if (event == mode) throw failure;
    }
    public static int capacity(int first, int second) {
        capacityHeld = entryLock != null && Thread.holdsLock(entryLock);
        hit(2); return first + second;
    }
    public static void inside(int first, int second) {
        width = first; height = second;
        insideHeld = entryLock != null && Thread.holdsLock(entryLock);
        hit(3);
        if (mode == 11) lock = new Object();
    }
    public static void after() {
        afterHeld = entryLock != null && Thread.holdsLock(entryLock);
        hit(5);
    }
}
