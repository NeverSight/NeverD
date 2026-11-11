package fixture;
public class Child<C> extends Parent<C> {
    public C value;
    public C make() {
        Trace.events = Trace.events * 10 + 3;
        if (Trace.targetFailure != null) throw Trace.targetFailure;
        return value;
    }
}
