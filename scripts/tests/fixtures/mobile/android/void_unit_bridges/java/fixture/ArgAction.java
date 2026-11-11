package fixture;

public interface ArgAction<T, R> {
    R invoke(T value);
}
