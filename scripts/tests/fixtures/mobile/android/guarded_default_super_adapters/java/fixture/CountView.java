package fixture;

public interface CountView<R> {
    R get(Object key);
    default void forEach(java.util.function.Consumer<? super R> action) {
        action.accept(get(null));
    }
    default R getOrDefault(Object key, R fallback) {
        R value = get(key);
        return value == null ? fallback : value;
    }
}
