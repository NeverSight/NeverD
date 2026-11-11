package fixture;
public final class Impl implements Narrow {
    public Item current;
    public RuntimeException failure;
    public int calls;
    public Item next() {
        calls++;
        if (failure != null) throw failure;
        return current;
    }
    public boolean hasNext() { return current != null; }
}
