package fixture;
public final class Outer {
    static { Trace.hit(1); }
    public final Inner child;
    public Outer(Inner child) { Trace.hit(8); this.child=child; Trace.hit(9); }
}
