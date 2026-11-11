package fixture;
public final class Bundle {
    static { Trace.step(1); }
    public Reader<Object> first, second;
    public Object direct;
    public Bundle(Reader<Object> first, Reader<Object> second, Object direct) {
        Trace.step(5); this.first = first; this.second = second; this.direct = direct;
    }
}
