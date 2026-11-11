package fixture;
public final class Core extends Parent<Object> implements Marker {
    public final long[] values;
    public int calls, index;
    public Object marker, observed;
    public RuntimeException failure;
    public Core(long[] values) { this.values = values; }
    public long readValue(int requested) {
        calls++;
        index = requested;
        observed = marker;
        if (marker instanceof int[]) ((int[]) marker)[0] += 3;
        if (failure != null) throw failure;
        return values[requested];
    }
    public Object get(int index) { return Box.box(readValue(index)); }
}
