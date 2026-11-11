package fixture;

public interface Root<T> {
    T echo(T value);
    T[] array(T[] values);
}
