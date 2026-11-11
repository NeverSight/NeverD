package fixture;
public final class Core {
    public final Object first;
    public final Object second;
    public Core(Object value) {
        Trace.event(1);
        first = value;
        Trace.escaped = this;
        Trace.event(2);
        if (Trace.mode == 0) {
            Trace.event(3);
            second = value;
            Trace.event(4);
        } else {
            Trace.event(5);
            throw Trace.failure;
        }
    }
}
