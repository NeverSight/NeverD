package fixture;

public interface Contract<E> {
    void insert(int index, E value);
    E current();
    E tail();
}
