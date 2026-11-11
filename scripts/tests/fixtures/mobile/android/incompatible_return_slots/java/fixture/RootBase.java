package fixture;

public interface RootBase<T> {
    default boolean remove(Object key, Object value) { Trace.mark(1); return key == value; }
    default T put(T value) { Trace.mark(2); return value; }
    default void clear() { Trace.mark(3); }
}
