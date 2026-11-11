package fixture;
public abstract class Provider<E, C extends Box<E>> extends Base<C, E> {
    public int calls;
    public E last;
    public RuntimeException failure;
    protected final void insert(C box, int index, E value) {
        calls++; last = value;
        if (failure != null) throw failure;
        box.index = index; box.value = value;
    }
    public void typed(C box, int index, E value) { insert(box, index, value); }
}
