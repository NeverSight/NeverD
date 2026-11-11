package fixture;
public final class Target {
    static { Trace.event(1); }
    public final Object selected;
    public Target(Object value) {
        Trace.event(7);
        selected = value;
    }
}
