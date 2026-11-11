package fixture;

public class Derived<E> extends Cursor<E> {
    public int calls;

    public Derived(E value) {
        super(0);
        this.value = value;
    }

    public E next() {
        calls++;
        return super.next();
    }
}
