package fixture;
public abstract class Base {
    public final Object token;
    static { Trace.hit(1); }
    public Base(Object value) {
        Trace.seenToken = value;
        Trace.hit(2);
        token = value;
    }
    public abstract Menu getItem();
}
