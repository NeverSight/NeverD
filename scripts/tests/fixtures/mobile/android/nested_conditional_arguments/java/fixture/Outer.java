package fixture;
public final class Outer {
    static { Trace.hit(1); }
    public final Inner value;
    public Outer(Inner value) { Trace.hit(7);this.value=value;Trace.outer=this;Trace.hit(8); }
}
