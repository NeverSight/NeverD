package fixture;

import java.util.Map;

public class Node<K, V> implements Map.Entry<K, V> {
    public K key;
    public V value;

    public Node(K key, V value) {
        this.key = key;
        this.value = value;
    }

    public K getKey() {
        return key;
    }

    public V getValue() {
        return value;
    }

    public V setValue(V next) {
        V old = value;
        value = next;
        return old;
    }

    public boolean equals(Object other) {
        return this == other;
    }

    public int hashCode() {
        return 7;
    }
}
