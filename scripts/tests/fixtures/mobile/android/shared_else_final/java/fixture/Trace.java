package fixture;
public final class Trace {
    public static boolean first, second;
    public static int failAt;
    public static RuntimeException failure;
    public static long events;
    public static Object chosen;
    public static Core core;
    public static boolean[] seen;
    public static Object[] selectedAt;
    public static int[] numberAt;
    public static void hit(int event) {
        events = events * 17 + event;
        if (event == failAt) throw failure;
    }
    public static boolean first() { hit(2); return first; }
    public static boolean second() { hit(3); return second; }
    public static Object choose(Value value) { hit(7); return chosen; }
    public static void observe(Core value, int event) {
        core = value;
        seen[event] = true;
        selectedAt[event] = value.selected;
        numberAt[event] = value.number;
        hit(event);
    }
}
