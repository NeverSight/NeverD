package fixture;
public final class Target {
    static { Trace.event(1); }
    public final Object value;
    public final long bits;
    public Target(Object value,long bits) {
        Trace.event(6);
        this.value=value; this.bits=bits;
    }
}
