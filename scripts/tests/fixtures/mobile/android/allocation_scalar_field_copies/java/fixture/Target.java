package fixture;
public final class Target {
    static {
        Trace.step(1);
        if (Trace.owner != null) Trace.owner.value = !Trace.owner.value;
        Provider.staticFlag = !Provider.staticFlag;
    }
    public final boolean first, second;
    public final int input, alias;
    public final long bits;
    public final float floating;
    public final double fraction;
    public Target(boolean first, boolean second, int input, int alias, long bits, float floating, double fraction) {
        this.first = first; this.second = second; this.input = input; this.alias = alias;
        this.bits = bits; this.floating = floating; this.fraction = fraction;
        Trace.seen = this; Trace.step(3);
        if (Trace.failBody) throw Trace.failure;
        if (Trace.owner != null) Trace.owner.value = !first;
        Provider.staticFlag = !first;
    }
}
