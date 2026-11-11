package fixture;

public final class Owner<E> {
    public E value;
    public boolean ready;
    public long stamp;
    public int reads;
    public RuntimeException failure;
    public Owner(E value) { this.value = value; }
    public Object read() {
        ++reads;
        if (failure != null) throw failure;
        return value;
    }
}
