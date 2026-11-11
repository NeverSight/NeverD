package fixture;

public interface Action<T, R> {
    R invoke(T value);
}
