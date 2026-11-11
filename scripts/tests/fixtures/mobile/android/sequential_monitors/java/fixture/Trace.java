package fixture;
public final class Trace {
    public static long events;
    public static int failAt, baseMask;
    public static boolean early, skip;
    public static Object seen;
    public static RuntimeException failure;
    public static void check(Object first, Object second, Object input, int event, int acquired) {
        int expected = baseMask | acquired;
        if (first == second && first != null && expected != 0) expected = 3;
        if (first != null && Thread.holdsLock(first) != ((expected & 1) != 0))
            throw new AssertionError("owned sequential monitor contract: first lock");
        if (second != null && Thread.holdsLock(second) != ((expected & 2) != 0))
            throw new AssertionError("owned sequential monitor contract: second lock");
        events = events * 10 + event;
        seen = input;
        if (event == failAt) throw failure;
    }
}
