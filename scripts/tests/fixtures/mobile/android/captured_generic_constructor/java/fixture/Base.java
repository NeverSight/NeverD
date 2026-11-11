package fixture;
public class Base implements Tag<Object> {
    static { Trace.step(1); }
    public Sink<Object> sink;
    public int arity;
    public long wide;
    public Base(int count, Sink<Object> value, long bits) {
        Trace.step(2);
        arity = count; wide = bits; sink = value;
        Trace.seen = this;
        if (value != null) value.take(this);
        Trace.step(3);
        if (Trace.failBody) throw Trace.failure;
    }
    public Base(int count, Object value, long bits) { Trace.step(8); }
}
