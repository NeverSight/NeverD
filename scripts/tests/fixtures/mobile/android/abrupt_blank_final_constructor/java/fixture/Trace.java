package fixture;

public class Trace {
    public static Object owner;
    public static Throwable failure;
    public static long events;
    public static Runnable action;

    public static void hit(int event) {
        events = events * 10 + event;
        action.run();
    }
}
