package fixture;
public class Root<K, V> {
    public Object lastKey;
    public Object lastValue;
    public Object stored;
    public int calls;
    public int rootCalls;
    public int overrideCalls;
    public RuntimeException failure;
    public Root() {}
    public boolean contains(K key) {
        rootCalls++;
        lastKey = key;
        if (failure != null) throw failure;
        return stored == key;
    }
    @SuppressWarnings("unchecked")
    public V get(K key) {
        rootCalls++;
        lastKey = key;
        if (failure != null) throw failure;
        return (V) stored;
    }
    public boolean remove(Object key, Object value) {
        calls++;
        lastKey = key;
        lastValue = value;
        if (failure != null) throw failure;
        if (stored != value) return false;
        stored = null;
        return true;
    }
}
