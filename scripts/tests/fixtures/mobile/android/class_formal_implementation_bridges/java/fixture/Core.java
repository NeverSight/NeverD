package fixture;
public class Core<T extends Comparable<? super T>, U extends Comparable<? super U>> implements Api<T> {
    public T bound, seen;
    public int[] marker, observed;
    public int calls;
    public RuntimeException failure;
    public boolean contains(T value) { return Dispatch.contains(this, value); }
    public boolean isEmpty() { return Dispatch.empty(this); }
    public T lower() { return bound; }
    public T observe(T value) {
        calls++;
        observed = marker;
        marker[0] += 3;
        seen = value;
        if (failure != null) throw failure;
        return value;
    }
}
