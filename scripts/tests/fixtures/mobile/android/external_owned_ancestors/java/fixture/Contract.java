package fixture;

public interface Contract<T> {
    T value(T value);

    default T relay(T value) {
        Trace.defaults++;
        return value(value);
    }
}
