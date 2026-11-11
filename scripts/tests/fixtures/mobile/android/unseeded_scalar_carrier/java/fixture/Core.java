package fixture;
public class Core {
    public static boolean call(Carrier<?> values, Object input) { return Finder.test(values, input); }
    public static boolean guarded(Carrier<?> values, Object input) {
        try { return Finder.test(values, input); }
        catch (Throwable failure) { Trace.lastFailure = failure; return false; }
    }
    public static boolean locked(Carrier<?> values, Object input) {
        synchronized (Trace.lock) { return Finder.test(values, input); }
    }
}
