package fixture;
import java.util.NoSuchElementException;

public final class Core implements Api<Object>, Marker {
    private final long[] values;
    public int index;
    public int calls;
    public Object marker;
    public Object observed;
    public RuntimeException failure;

    public Core(long[] values) { this.values = values; }
    public long readValue() {
        calls++;
        observed = marker;
        if (marker instanceof int[]) ((int[]) marker)[0] += 3;
        if (failure != null) throw failure;
        if (index >= values.length) throw new NoSuchElementException("owned-end");
        return values[index++];
    }
    @Override public Object next() { return Box.box(readValue()); }
    @Override public boolean hasNext() { return index < values.length; }
    @Override public void remove() { throw new UnsupportedOperationException("owned-remove"); }
    @Override public <R> R echo(R input) { return input; }
}
