package fixture;
public final class Trace {
    public static long events;
    public static boolean inputFlag;
    public static int requestBits, bits;
    public static Value left, right, configured;
    public static Input tail, firstSeen, secondSeen;
    public static Object self;
    public static Runnable configure, prepare, root, base;
    public static void hit(int event) { events = events * 10 + event; }
}
