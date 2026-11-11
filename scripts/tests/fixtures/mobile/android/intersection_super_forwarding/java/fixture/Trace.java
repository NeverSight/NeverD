package fixture;
public final class Trace {
    public static long events;
    public static Object receiver, value;
    public static Runnable before, after;
    public static void hit(int event) { events = events * 10 + event; }
}
