package fixture;
public final class Primary implements Delegate {
    static { Trace.hit(7); }
    public Object marker;
    public Primary(Object value) { Trace.allocated = this; Trace.hit(12); marker = value; }
    public Object read() { Trace.hit(11); return Trace.payload; }
}
