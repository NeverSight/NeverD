package fixture;
public final class Trace {
    public static long events;
    public static int failAt;
    public static RuntimeException failure;
    public static Object observed;
    public static void event(int point) {
        events=events*10+point;
        if (point==failAt) throw failure;
    }
}
