package fixture;
public final class Outer {
    static { Trace.hit(1); }
    public final String message;
    public final Object tag;
    public Outer(String value, Object token) {
        Trace.hit(8);
        message = value; tag = token; Trace.outer = this;
        Trace.hit(9);
    }
}
