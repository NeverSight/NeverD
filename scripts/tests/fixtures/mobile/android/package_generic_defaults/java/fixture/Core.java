package fixture;
public class Core {
    public static String create(Object input, String label) { return Provider.pick(input, label, Trace.fallback); }
    public static String guarded(Object input, String label) {
        try { return Provider.pick(input, label, Trace.fallback); }
        catch (Throwable failure) { Trace.last = failure; return null; }
    }
    public static String locked(Object input, String label) {
        synchronized (Trace.lock) { return Provider.pick(input, label, Trace.fallback); }
    }
}
