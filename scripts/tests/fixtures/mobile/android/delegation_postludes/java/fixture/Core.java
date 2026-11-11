package fixture;
public final class Core {
    public final Object base;
    public int mark;
    public long stamp;
    public Core(Object value) {
        Trace.event(1);
        base = value;
        Trace.current = this;
        Trace.event(2);
    }
    public Core(Object value, int mode) {
        this(value);
        mark = mode;
        Trace.event(3);
    }
    public Core(Object value, int mode, long bits) {
        this(value, mode);
        stamp = bits;
        Trace.event(4);
    }
}
