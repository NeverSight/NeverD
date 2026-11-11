package fixture;

public interface Contract<T> {
    T get(Carrier<T, String> carrier);
}
