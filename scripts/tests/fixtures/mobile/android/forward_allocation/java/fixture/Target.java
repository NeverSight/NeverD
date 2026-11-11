package fixture;
public final class Target {
    static { Trace.hit(1); }
    public Object first;
    public Target() {
        Trace.partial = this;
        first = Trace.value;
        Trace.hit(2);
    }
}
