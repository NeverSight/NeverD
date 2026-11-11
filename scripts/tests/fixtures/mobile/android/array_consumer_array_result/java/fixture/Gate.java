package fixture;

public interface Gate<T> {
    boolean accept(T value);
}
