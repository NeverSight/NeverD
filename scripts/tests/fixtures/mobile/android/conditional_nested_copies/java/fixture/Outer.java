package fixture;
public final class Outer {
    static { Trace.hit(1); }
    public final Object tag;
    public final long first;
    public final long second;
    public final Object value;
    public Outer(Object tag, long first, long second, Object value) {
        this.tag = tag; this.first = first; this.second = second; this.value = value;
        Trace.outer = this; Trace.hit(6);
    }
    public Outer(Object tag, double first, double second, Object value) {
        this.tag = tag;
        this.first = Double.doubleToRawLongBits(first);
        this.second = Double.doubleToRawLongBits(second); this.value = value;
        Trace.outer = this; Trace.hit(6);
    }
}
