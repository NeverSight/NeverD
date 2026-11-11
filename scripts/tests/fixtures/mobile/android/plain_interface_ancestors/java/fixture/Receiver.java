package fixture;

public class Receiver<T extends Tag> implements Leaf<T> {
    public Object contextValue;
    public T last;
    public RuntimeException failure;
    public int calls;
    public Receiver() {}
    public Object context() {
        calls++;
        if (failure != null) throw failure;
        return contextValue;
    }
    public T echo(T value) {
        calls++; last = value;
        if (failure != null) throw failure;
        return value;
    }
}
