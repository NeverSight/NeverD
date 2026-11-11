package fixture;

public interface Api<K> {
    void touch(Class<?> type, K key);

    default <E extends Enum<E>> E choose(Class<E> type, K key, E fallback) {
        touch(type, key);
        return fallback;
    }
}
