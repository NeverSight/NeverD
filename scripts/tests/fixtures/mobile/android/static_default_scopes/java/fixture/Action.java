package fixture;

public interface Action<T> {
    Object apply(Holder holder);
}
