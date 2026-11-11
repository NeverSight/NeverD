package fixture;
public interface Api<E> {
    boolean add(E item);
    default boolean insert(E item) { return add(item); }
}
