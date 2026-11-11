package fixture;
public final class Inner {
    static { Trace.hit(2); }
    public final Object tag;
    public String buffer = "";
    public int calls;
    public Inner() { Trace.hit(3); tag = null; Trace.inner = this; }
    public Inner(Object value) { Trace.hit(3); tag = value; Trace.inner = this; }
    public Inner append(String part) {
        calls++;
        Trace.hit(calls == 1 ? 4 : 6);
        buffer = buffer + part;
        return this;
    }
    public String text() { Trace.hit(7); return buffer; }
}
