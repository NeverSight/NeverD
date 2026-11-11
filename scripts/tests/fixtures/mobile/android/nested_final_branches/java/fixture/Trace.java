package fixture;
public final class Trace {
    public static long events;
    public static int failAt;
    public static RuntimeException failure;
    public static Core core;
    public static Object allocated, token, payload;
    public static Delegate provided;
    public static boolean[] seen;
    public static Object[] selectedAt, extraAt;
    public static int[] firstAt, secondAt;
    public static long[] stampAt;
    public static void hit(int event) {
        events = events * 17 + event;
        if (failAt == event) throw failure;
    }
    public static void observe(Core value, int event) {
        core = value;
        seen[event] = true;
        selectedAt[event] = value.selected; extraAt[event] = value.extra;
        firstAt[event] = value.first; secondAt[event] = value.second; stampAt[event] = value.stamp;
        hit(event);
    }
}
