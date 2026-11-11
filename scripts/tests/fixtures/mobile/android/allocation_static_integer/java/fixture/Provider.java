package fixture;
public final class Provider {
    public Object context() {
        Trace.calls++;
        Trace.event(2);
        if (Trace.calls == 1) Trace.initialNumber = Trace.afterCall;
        else Fields.number = Trace.afterCall;
        return Trace.owner;
    }
}
