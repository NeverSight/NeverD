package fixture;
public final class Owner<K,V> {
    static { Trace.hit(8); }
    public final K initialKey;
    public final V initialValue;
    public Owner(K key,V value) { initialKey=key;initialValue=value; }
    public View<K,V> next() {
        return new View<K,V>() {
            private final K key=initialKey;
            private V value=initialValue;
            public K getKey() { return key; }
            public V getValue() { Trace.hit(2);return value; }
            public V peek() { return value; }
            public void assign(V input) { this.value=input; }
            public void restore(V input) { assign(input); }
            public V setValue(V input) {
                Trace.hit(1);
                V previous=getValue();
                assign(input);
                Trace.hit(3);
                Trace.callback(this,getKey(),input);
                return previous;
            }
        };
    }
}
