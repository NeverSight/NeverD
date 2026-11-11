package fixture;
public final class Child<T> extends Base<T> {
    static { Trace.hit(2); }
    public Object marker;
    public Child(Object marker) {
        super(new Adapter());
        this.marker = marker;
        Trace.child = this;
        Trace.hit(7);
    }
}
