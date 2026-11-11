package fixture;
public abstract class Provider<E, C extends Box<E>> extends Base<C, E> {
    public int calls;
    public Object seen;
    public Box<E> storage;
    public RuntimeException failure;
    protected Box<E> read(C input) {
        calls++; seen = input;
        if (failure != null) throw failure;
        return storage;
    }
}
