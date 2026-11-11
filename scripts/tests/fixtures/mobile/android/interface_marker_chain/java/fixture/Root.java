package fixture;
public interface Root<T> extends Marker {
    T echo(T value);
    T[] array(T[] values);
}
