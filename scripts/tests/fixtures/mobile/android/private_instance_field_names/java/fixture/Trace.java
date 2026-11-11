package fixture;
public final class Trace {
    public static long events;
    public static int failAt;
    public static Runnable action;
    public static Object created, seen, seenInput;
    public static void hit(int event) {
        events = events * 100 + event;
        if (event == failAt) action.run();
    }
}
