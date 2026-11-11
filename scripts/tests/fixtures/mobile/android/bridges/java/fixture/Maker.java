package fixture;

public interface Maker<T> {
    T make(int value);
    T[] many(int size);
    T echo(T value);
}
