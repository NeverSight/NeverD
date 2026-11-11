package fixture;
public final class Core {
    public static Outer make(boolean choose, Object tag, long first, long second) {
        return new Outer(tag, first, second,
            Trace.choose(choose) ? new Inner(tag, first, second) : Trace.fallback());
    }
    public static Outer floating(boolean choose, Object tag, double first, double second) {
        return new Outer(tag, first, second,
            Trace.choose(choose) ? new Inner(tag, first, second) : Trace.fallback());
    }
}
