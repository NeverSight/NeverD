package fixture;
public interface Base<E> {
    default void begin(E value) {
        throw new IllegalStateException("owned base default must be shadowed");
    }
    default E first() {
        throw new IllegalStateException("owned base default must be shadowed");
    }
}
