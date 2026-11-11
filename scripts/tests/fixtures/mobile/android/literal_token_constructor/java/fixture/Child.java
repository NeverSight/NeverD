package fixture;
public final class Child extends Base {
    static { Trace.step(7); }
    public Class<?> observed;
    public Child(Context input) {
        super("owned", Probe.class, input, 29, 0x8000000000000007L, -0.0d, input);
        observed = type;
        Trace.step(5);
    }
}
