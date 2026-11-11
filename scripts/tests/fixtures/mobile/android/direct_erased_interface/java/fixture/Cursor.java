package fixture;

public final class Cursor<E> {
    private final Object value;
    public int positions;
    public Cursor(E value) { this.value = value; }
    public E next() {
        ++positions;
        return (E) value;
    }
}
