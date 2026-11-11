package fixture;

public interface Extended<E> extends Cursor<E> {
    void accept(E value);
}
