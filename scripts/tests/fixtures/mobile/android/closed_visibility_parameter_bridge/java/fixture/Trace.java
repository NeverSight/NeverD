package fixture;
public final class Trace {
    public static long events;
    public static Object receiver;
    public static Object argument;
    public static Runnable action;
    public static Runnable futureAction;
    public static void hit(int stage) { events = events * 10 + stage; }
}
