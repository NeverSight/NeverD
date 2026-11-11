package fixture;
import java.util.Collection;
import java.util.Iterator;
public final class Core implements Api<Object>, Marker {
    public int calls; public int observed; public int[] marker; public RuntimeException failure;
    public Iterator<Object> cursor;
    public Core() {}
    private void touch() {
        calls++; observed = marker == null ? -9 : marker[0];
        if (marker != null) marker[0] += 3;
        if (failure != null) throw failure;
    }
    public boolean add(Object input) { throw new UnsupportedOperationException("owned immutable operation"); }
    public boolean addScalar(int input) { throw new UnsupportedOperationException("owned immutable operation"); }
    public boolean bulk(Collection<? extends Object> input) { throw new UnsupportedOperationException("owned immutable operation"); }
    public boolean containsAll(Collection<?> input) { touch(); return input == null; }
    public <R, S> R echo(R input, S tag) { touch(); return input; }
    public <R> R[] array(R[] input) { touch(); return input; }
    public Iterator<Object> values() { touch(); return cursor; }
}
