package fixture;
public class Provider {
    static { Trace.fallback = "after initialization"; Trace.hit(4); }
    static <T> T pick(Object input, String label, T fallback) {
        Object result = null;
        boolean complete = false;
        try { result = Trace.read(input, label); complete = true; }
        catch (IllegalArgumentException failure) { Trace.last = failure; Trace.hit(3); }
        finally { Trace.result = result; Trace.complete = complete; Trace.hit(5); if (result == null && complete) result = fallback; }
        return (T) result;
    }
}
