package fixture;
public final class Inner {
    static { Trace.hit(3); }
    public final Object tag;
    public final long first;
    public final long second;
    public Inner(Object tag, long first, long second) {
        this.tag = tag; this.first = first; this.second = second;
        Trace.inner = this; Trace.hit(4);
    }
    public Inner(Object tag, double first, double second) {
        this.tag = tag;
        this.first = Double.doubleToRawLongBits(first);
        this.second = Double.doubleToRawLongBits(second);
        Trace.inner = this; Trace.hit(4);
    }
}
