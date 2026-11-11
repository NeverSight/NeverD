package fixture;

import java.util.Iterator;
import java.util.Map;

public class CursorEntry<K, V> implements Iterator<Map.Entry<K, V>>, Map.Entry<K, V> {
    public K key;
    public V value;
    public boolean consumed;
    public int nextCalls;
    public int hasNextCalls;
    public int keyCalls;
    public int setCalls;
    public int removeCalls;
    public RuntimeException failure;

    public CursorEntry(K key, V value) {
        this.key = key;
        this.value = value;
    }

    public K getKey() {
        keyCalls++;
        if (failure != null) throw failure;
        return key;
    }

    public V getValue() { return value; }

    public V setValue(V next) {
        setCalls++;
        if (failure != null) throw failure;
        V old = value;
        value = next;
        return old;
    }

    public boolean equals(Object other) { return this == other; }
    public int hashCode() { return 17; }

    public boolean hasNext() {
        hasNextCalls++;
        return !consumed;
    }

    public Map.Entry<K, V> next() {
        nextCalls++;
        consumed = true;
        if (failure != null) throw failure;
        return this;
    }

    public void remove() {
        removeCalls++;
        if (failure != null) throw failure;
        consumed = false;
    }
}
