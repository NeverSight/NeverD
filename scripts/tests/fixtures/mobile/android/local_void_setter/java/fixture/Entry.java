package fixture;

public final class Entry<K,V> implements java.util.Map.Entry<K,V> {
    static { Trace.hit(8); }
    private final K key;
    private V value;
    public Entry(K key, V value) { this.key = key; this.value = value; }
    public K getKey() { return key; }
    public V getValue() { Trace.hit(2); return value; }
    public V peek() { return value; }
    public void assign(V input) { this.value = input; }
    public void restore(V input) { assign(input); }
    public V setValue(V input) {
        Trace.hit(1);
        V previous = getValue();
        assign(input);
        Trace.hit(3);
        Trace.callback(this, getKey(), input);
        return previous;
    }
}
