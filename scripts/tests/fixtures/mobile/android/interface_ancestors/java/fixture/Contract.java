package fixture;

public interface Contract<T, U> extends Left<T>, Right<T> {
    T echo(T value, int delta);
    T[] newArray(int size);
}
