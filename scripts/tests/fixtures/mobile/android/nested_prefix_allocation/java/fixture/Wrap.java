package fixture;
public final class Wrap implements Value {
    public final Value child;
    public final Object tail;
    static { Trace.hit(1); Trace.outerInit.run(); Trace.hit(2); }
    public Wrap(Value child,Object tail) {
        Trace.hit(7); Trace.wrapSelf=this;
        this.child=child; this.tail=tail;
        Trace.outerAction.run(); Trace.hit(8);
    }
}
