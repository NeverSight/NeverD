package fixture;

public interface Cursor<E> {
    boolean hasNext();
    E next();
    <T> T[] echo(T[] values);
}
