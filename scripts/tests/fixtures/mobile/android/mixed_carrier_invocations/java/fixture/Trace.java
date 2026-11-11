package fixture;
public final class Trace {
    public static long events, flags;
    public static int failAt, reads, saves;
    public static RuntimeException failure;
    public static Object[] inputs;
    public static Object key, saved;
    public static void hit(int event) {
        events = events * 10 + event;
        if (failAt == event) throw failure;
    }
}
