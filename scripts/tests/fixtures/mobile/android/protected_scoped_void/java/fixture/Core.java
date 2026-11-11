package fixture;
public final class Core<E> extends Base<Box<E>, E> {
    public int calls;
    public E last;
    public RuntimeException failure;
    protected void insert(Box<E> box, int index, E value) {
        calls++; last = value;
        if (failure != null) throw failure;
        box.index = index; box.value = value;
    }
    public void typed(Box<E> box, int index, E value) { insert(box, index, value); }
}
