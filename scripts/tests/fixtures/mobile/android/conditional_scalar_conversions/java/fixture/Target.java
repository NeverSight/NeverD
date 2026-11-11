package fixture;
public final class Target {
    static { Trace.event(1); }
    public final Object seed;
    public final int converted;
    public final int tail;
    public Target(Object seed, int converted, int tail) {
        Trace.event(5);
        this.seed = seed; this.converted = converted; this.tail = tail;
    }
}
