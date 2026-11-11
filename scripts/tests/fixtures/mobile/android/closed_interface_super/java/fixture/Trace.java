package fixture;
public class Trace {
    public static int events, failAt, swapAt;
    public static Object receiver;
    public static Item item;
    public static Result fallback, late, constructed;
    public static RuntimeException failure;
    public static Error error;
    public static void step(int event) {
        events = events * 10 + event;
        if (swapAt == event) fallback = late;
        if (failAt == event) throw failure;
        if (failAt == -event) throw error;
    }
}
