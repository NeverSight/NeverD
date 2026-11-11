package fixture;

public interface Producer<T> extends Marker<T> {
    T read();
    T echo(T value);
}
