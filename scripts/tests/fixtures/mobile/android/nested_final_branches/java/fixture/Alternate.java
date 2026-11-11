package fixture;
public final class Alternate implements Delegate {
    static { Trace.hit(8); }
    public Object marker;
    public Alternate(Object value) { Trace.allocated = this; marker = value; Trace.hit(13); }
    public Object read() { Trace.hit(11); return Trace.payload; }
}
