package fixture;
public class Base {
    static { Trace.step(1); }
    public Sink<Object> sink;
    public Base(Sink<Object> value) {
        Trace.step(2);
        sink = value;
        Trace.seen = this;
        if (value != null) value.take(this);
        Trace.step(3);
        if (Trace.failBody) throw Trace.failure;
    }
    public Base(Object value) { Trace.step(8); }
}
