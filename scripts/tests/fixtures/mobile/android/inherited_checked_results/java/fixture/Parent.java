package fixture;
@SuppressWarnings("unchecked")
public class Parent<T> implements Generic<T> {
    public Object stored, seen;
    public int calls, index;
    public long stamp;
    public double fraction;
    public RuntimeException failure;
    public T fetch() {
        calls++; index = -1;
        if (failure != null) throw failure;
        return (T)stored;
    }
    public T fetch(int value) {
        calls++; index = value;
        if (failure != null) throw failure;
        return (T)stored;
    }
    public T fetch(long value, double part, Object token) {
        calls++; stamp = value; fraction = part; seen = token;
        if (failure != null) throw failure;
        return (T)stored;
    }
}
