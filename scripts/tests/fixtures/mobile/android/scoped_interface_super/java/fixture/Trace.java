package fixture;
public final class Trace {
    public static int events, fail, first, second;
    public static Object result, replacement, seen, after;
    public static Throwable failure;
    public static boolean wrong;
    private Trace() {}
    public static void hit(int step) {
        events = events * 10 + step;
        if (step == 4) result = replacement;
        if (step == fail) {
            if (failure instanceof Error) throw (Error)failure;
            throw (RuntimeException)failure;
        }
    }
}
