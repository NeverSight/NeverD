package fixture;

public interface Action<T> {
    void accept(T value);
}
