package fixture;
public final class Target {
    static { Trace.event(1); }
    public final Object seed;
    public final long bits;
    public final int tail;
    public Target(Object seed,long bits,int tail) { Trace.event(5); this.seed=seed; this.bits=bits; this.tail=tail; }
}
