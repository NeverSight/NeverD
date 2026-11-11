package fixture;
public final class Target {
    static { Trace.events = Trace.events * 10 + 1; if (Trace.failAt == 1) throw Trace.failure; }
    public Class<?> type;
    public Object item;
    public Target(Class<?> kind, Object value) {
        Trace.events = Trace.events * 10 + 4; Trace.ctors++;
        if (Trace.failAt == 4) throw Trace.failure;
        type = kind; item = value;
    }
}
