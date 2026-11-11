package fixture;
public class Root<K, V> implements Api<K, V> {
    public V put(K key, V value) { return value; }
}
