package fixture;
public interface Api extends Root {
    default Payload getStyle() {
        Trace.calls++;
        Trace.events = Trace.events * 10 + 1;
        Payload result = Trace.items[Trace.index];
        if (Trace.failure != null) throw Trace.failure;
        Trace.events = Trace.events * 10 + 2;
        return result;
    }
}
