package fixture;

public interface Creator<T> {
    T make(int value);
    T[] many(int size);
    T echo(T value);
}
