package fixture;

public interface Root<T> {
    T get(Object key);
    default T getOrDefault(Object key, T fallback) {
        Trace.mark(1);
        T found = get(key);
        return found != null ? found : fallback;
    }
}
