package fixture;

public final class Trace {
    public static int events;
    public static void mark(int event) { events = events * 10 + event; }
}
