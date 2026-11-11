package fixture;
public class Base {
    static { Trace.hit(1); }
    public final Object tag;
    public Base(Object tag) {
        Trace.hit(3); this.tag = tag; Trace.seen = outer(); Trace.hit(4);
    }
    public Object outer() { return null; }
}
