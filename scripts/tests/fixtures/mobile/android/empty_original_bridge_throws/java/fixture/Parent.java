package fixture;
public class Parent<T> implements Api<T> {
    public int visits;
    public Object result;
    public T seenParent;
    public int[] seenMarker;
    public First first;
    public Second second;
    public Box visit(T value, int[] marker) throws First, Second {
        return observe((Box) value, marker);
    }
    public Box observe(Box value, int[] marker) throws First, Second {
        visits++;
        seenParent = (T) value;
        seenMarker = marker;
        marker[0] += 3;
        if (first != null) throw first;
        if (second != null) throw second;
        return (Box) result;
    }
}
