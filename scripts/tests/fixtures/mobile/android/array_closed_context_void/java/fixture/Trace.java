package fixture;
public class Trace {
    public static long events;
    public static int entries, exits, failAt;
    public static RuntimeException failure;
    public static Error error;
    public static void hit(int event) {
        events = events * 10 + event;
        if (event == 1) entries++;
        if (event == 2) exits++;
        if (event == failAt) throw failure;
    }
}
