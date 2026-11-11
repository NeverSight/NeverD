package fixture;
public final class Trace {
    public static int events, fail, derived, stampValue;
    public static Throwable failure;
    public static Object returned, alternate, getterReceiver, stampReceiver, element;
    public static Parent<?,?,?> parent;
    public static Builder<?,?> builder, other;
    public static Path[] path, replacementPath;
    public static Path changed;
    private Trace() {}
    public static void hit(int step) {
        events = events * 10 + step;
        if (step == 2) returned = alternate;
        if (step == fail) {
            if (failure instanceof Error) throw (Error)failure;
            throw (RuntimeException)failure;
        }
    }
}
