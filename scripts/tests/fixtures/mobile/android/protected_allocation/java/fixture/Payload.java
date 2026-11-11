package fixture;
public final class Payload {
    static { Trace.step(1); }
    public Object value;
    public Payload(Object input) { Trace.step(3); value = input; }
}
