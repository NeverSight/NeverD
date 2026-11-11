package fixture;
public final class Core {
    public static Object run(Object first, Object second, Object input) {
        Trace.check(first, second, input, 1, 0);
        synchronized (first) {
            Trace.check(first, second, input, 2, 1);
            if (Trace.early) return input;
        }
        Trace.check(first, second, input, 3, 0);
        if (Trace.skip) return input;
        synchronized (second) {
            Trace.check(first, second, input, 4, 2);
        }
        Trace.check(first, second, input, 5, 0);
        return input;
    }
}
