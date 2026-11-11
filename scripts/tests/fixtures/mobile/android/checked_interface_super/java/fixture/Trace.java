package fixture;

public final class Trace {
    public static int events;
    public static int fail;
    public static Throwable failure;
    public static Object input;
    public static Object replacement;
    public static Object seen;
    public static Parent<?> owner;
    public static boolean wrong;

    private Trace() {}

    public static void hit(int value) {
        events = events * 10 + value;
        if (value == 1) input = replacement;
        if (value == fail) {
            if (failure instanceof Error) throw (Error)failure;
            throw (RuntimeException)failure;
        }
    }
}
