package fixture;

public class Receiver<S,V> implements Leaf<S,V> {
    public S value;
    public RuntimeException failure;
    public int calls;
    public Receiver() {}
    public S get(Object key) {
        calls++;
        Trace.mark(2);
        if (failure != null) throw failure;
        return value;
    }
}
