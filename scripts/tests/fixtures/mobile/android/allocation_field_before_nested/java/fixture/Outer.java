package fixture;
public final class Outer {
    static { Trace.hit(1); }
    public final Inner inner;
    public Outer(Inner inner) { Trace.hit(5); this.inner = inner; }
}
