package fixture;
import java.util.NoSuchElementException;

public abstract class Core implements Api<Object>, Marker {
    protected final long[] values;
    public int index;
    public int calls;
    public Object marker;
    public Object observed;
    public RuntimeException failure;

    public Core(long[] values) { this.values = values; }
    public abstract long readValue();
    public final Box typedNext() { return Box.box(readValue()); }
    @Override public Object next() { return Box.box(readValue()); }
    @Override public boolean hasNext() { return index < values.length; }
    @Override public void remove() { throw new UnsupportedOperationException("owned-remove"); }
    @Override public <R> R echo(R input) { return input; }
}
