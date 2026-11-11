package fixture;
public final class Trace {
    public static long trace;
    public static Runnable hook;
    public static void hit(int event) {
        trace = trace * 10 + event;
        Runnable callback = hook;
        if (callback != null) {
            hook = null;
            callback.run();
        }
    }
}
