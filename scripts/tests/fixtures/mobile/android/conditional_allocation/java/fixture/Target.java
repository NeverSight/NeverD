package fixture;
public final class Target {
    static { Trace.event(1); }
    public final long bits;
    public final Owner owner;
    public final Object value;
    public Target(long input, Owner source, Object data) {
        Trace.event(7);
        bits = input; owner = source; value = data;
    }
}
