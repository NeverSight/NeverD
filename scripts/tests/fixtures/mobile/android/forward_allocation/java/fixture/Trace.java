package fixture;
public final class Trace {
    public static Hook hook;
    public static long events;
    public static Object value;
    public static Target partial;
    public static RuntimeException caught;
    public static void hit(int value) {
        events = events * 10 + value;
        if (hook != null) hook.event(value);
    }
    public static String message() { hit(4); return "owned sibling"; }
}
