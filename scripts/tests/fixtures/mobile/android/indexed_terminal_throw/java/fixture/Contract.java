package fixture;
public interface Contract<E> extends Base<E> {
    boolean add(E value);
    void insert(int index, E value);
    E replace(int index, E value);
    E remove(int index);
    default void begin(E value) {
        throw new IllegalStateException("owned default must be shadowed");
    }
    default E first() {
        throw new IllegalStateException("owned default must be shadowed");
    }
}
