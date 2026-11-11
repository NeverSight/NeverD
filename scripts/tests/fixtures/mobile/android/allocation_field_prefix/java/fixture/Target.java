package fixture;
public final class Target {
    static { Trace.hit(1); }
    public final Value value;
    public final Inner inner;
    public Target(Value value, Inner inner) {
        Trace.targets++;
        Trace.hit(5);
        this.value = value;
        this.inner = inner;
    }
}
