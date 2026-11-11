package fixture;
public final class Inner {
    static {
        Trace.events = Trace.events * 10 + 2;
        if (Trace.failAt == 2) throw Trace.failure;
    }
    public Object value;
    public Inner(Object item) {
        Trace.events = Trace.events * 10 + 4; Trace.inners++;
        if (Trace.failAt == 4) throw Trace.failure;
        value = item;
    }
}
