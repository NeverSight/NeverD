package fixture;
public final class Leaf implements Value {
    public final Object value;
    static { Trace.hit(3); Trace.innerInit.run(); Trace.hit(4); }
    public Leaf(Object value) {
        Trace.hit(5); Trace.leafSelf=this;
        this.value=value;
        Trace.innerAction.run(); Trace.hit(6);
    }
}
