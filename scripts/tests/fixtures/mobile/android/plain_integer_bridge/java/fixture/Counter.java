package fixture;
public class Counter<E> implements CountView<E> {
    public int count;
    public int calls;
    public RuntimeException failure;
    public Counter() {}
    public int getCount() {
        calls++;
        if (failure != null) throw failure;
        return count;
    }
    public final int size() { return getCount(); }
}
