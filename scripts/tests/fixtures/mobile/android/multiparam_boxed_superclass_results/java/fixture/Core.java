package fixture;
public final class Core extends Parent<String,Object,Base<Object>> implements Api<Object>, Marker {
    public final byte[] values;
    public int calls;
    public Object marker, observed;
    public RuntimeException failure;
    public Core(byte[] values) { this.values = values; }
    protected byte[] readValue() {
        calls++; observed = marker;
        if (marker instanceof int[]) ((int[])marker)[0] += 3;
        if (failure != null) throw failure;
        return values;
    }
    public Object get() { return Box.box(readValue()); }
}
