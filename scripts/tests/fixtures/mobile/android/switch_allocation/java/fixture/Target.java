package fixture;
public final class Target {
    static { Trace.hit(3); }
    public Object first, second;
    public Target(Object one, Object two) {
        Trace.partial = this;
        first = one;
        Trace.hit(6);
        second = two;
        Trace.hit(8);
    }
}
