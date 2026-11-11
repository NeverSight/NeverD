package fixture;
public final class Core<E> extends Base<Box<E>, E> {
    public int calls;
    public int operation;
    public Box<?> seen;
    public Box<E> storage;
    public RuntimeException failure;
    protected Box<E> make() { calls++; operation = 0; if (failure != null) throw failure; return storage; }
    protected int size(Box<E> input) { calls++; operation = 1; seen = input; if (failure != null) throw failure; return input.count; }
    protected Box<E> copy(Box<E> input) { calls++; operation = 2; seen = input; if (failure != null) throw failure; return input; }
    protected Box<E> narrow(Box<? extends E> input) { calls++; operation = 3; seen = input; if (failure != null) throw failure; return storage; }
}
