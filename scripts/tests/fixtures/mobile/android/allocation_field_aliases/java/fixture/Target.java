package fixture;
public final class Target {
    static { Trace.step(1); }
    public final int first, second, mode;
    public final Value value, alias;
    public final long bits;
    public final float floating;
    public final double fraction;
    public Target(int first, int second, int mode, Value value, Value alias, long bits, float floating, double fraction) {
        this.first = first; this.second = second; this.mode = mode;
        this.value = value; this.alias = alias; this.bits = bits;
        this.floating = floating; this.fraction = fraction;
        Trace.seen = this; Trace.step(3);
        if (Trace.failBody) throw Trace.failure;
        Provider.value = Trace.replacement;
    }
}
