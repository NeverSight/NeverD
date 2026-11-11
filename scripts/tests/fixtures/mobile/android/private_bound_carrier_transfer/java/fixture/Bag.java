package fixture;

public final class Bag<E> implements Slot<E> {
    public E value;
    public Bag() {}
    public void put(E value) {
        Trace.hit(4, null, this);
        this.value = value;
        Trace.hit(5, null, this);
    }
}
