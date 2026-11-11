package fixture;
public interface Sink<T> {
    Object resume(T value);
}
