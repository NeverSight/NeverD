package fixture;

public interface Producer<T> {
    Cell<T, ? extends T> upper();
    Cell<T, ? super T> lower();
    Cell<T, ?>[] array();
    T identity(T value);
}
