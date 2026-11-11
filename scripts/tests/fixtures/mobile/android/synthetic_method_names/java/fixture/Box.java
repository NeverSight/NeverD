package fixture;
public final class Box {
    static { Trace.event(6); }
    public final Object value;
    public Box(Object input) { Trace.event(5); value = input; }
}
