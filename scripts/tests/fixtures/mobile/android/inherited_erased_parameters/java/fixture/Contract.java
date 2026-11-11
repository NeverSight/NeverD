package fixture;
public interface Contract<P, Q> {
    boolean contains(Object key);
    Q get(Object key);
    boolean containsAll(java.util.Collection<?> items);
    default boolean remove(Object key, Object value) {
        Trace.defaults++;
        throw new AssertionError("unexpected default");
    }
}
