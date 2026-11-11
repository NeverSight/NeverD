package fixture;
public final class Vector<V extends Bound> {
    public final Object owner;
    public final V value;
    public final long wide;
    public Vector(Object owner,V value,long wide) {
        Trace.partial=this;Trace.hit(5);
        this.owner=owner;this.value=value;this.wide=wide;Trace.hit(6);
    }
}
