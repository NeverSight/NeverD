package fixture;

public interface Watcher<E> {
    void accept(E value);
}
