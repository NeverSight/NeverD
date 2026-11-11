package fixture;
public interface Api<K, V> {
    V put(K key, V value);
}
