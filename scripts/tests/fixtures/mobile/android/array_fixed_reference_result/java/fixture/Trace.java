package fixture;
public class Trace {
    public static long events;
    public static int calls, actions, failAt, initializations, facadeInitializations;
    public static boolean flag, override, fatal;
    public static Object[] input, result, chosen;
    public static Runnable action;
    public static RuntimeException failure;
    public static Error error;
    public static void hit(int event) {
        events = events * 10 + event;
        if (event == failAt) {
            if (fatal) throw error;
            throw failure;
        }
    }
}
