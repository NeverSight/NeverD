package fixture;

public class Trace {
    public static long events;
    public static int failAt, seed;
    public static IllegalArgumentException failure, stepFailure;
    public static Throwable caught;
    public static Core published;

    public static void hit(int event) {
        events = events * 11 + event;
        if (event == failAt) throw failure;
    }
}
