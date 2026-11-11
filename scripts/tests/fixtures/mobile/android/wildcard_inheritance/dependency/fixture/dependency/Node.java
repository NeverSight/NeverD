package fixture.dependency;

public final class Node<E> implements Leaf<E> {
    public E stored;
    public int reads;
    public RuntimeException failure;
    public Node(E stored) { this.stored = stored; }
    public Object value() {
        ++reads;
        if (failure != null) throw failure;
        return stored;
    }
}
