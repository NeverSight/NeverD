package fixture;
public interface Provider<K, V> extends Root<K, Entry<K, V>> {
    default Entry<K, V> put(K key, Entry<K, V> value) {
        Trace.calls++;
        Trace.key = key;
        Trace.value = value;
        if (Trace.failure != null) throw Trace.failure;
        return value;
    }
}
