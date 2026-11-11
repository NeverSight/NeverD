package fixture;
public final class Trace {
    public static Hook hook;
    public static long events;
    public static Object first, second;
    public static Target partial;
    public static int selected;
    public static void hit(int value) {
        events = events * 10 + value;
        if (hook != null) hook.event(value);
    }
    public static Object capture(Object value) { hit(1); return value; }
    public static int select(int value) { hit(2); return value; }
}
