package fixture;

import java.util.AbstractSet;
import java.util.Iterator;
import java.util.Map;

public class SetView<K, V> extends AbstractSet<Map.Entry<K, V>> {
    public Node<K, V> entry;
    public int count;
    public int sizeCalls;
    public int iteratorCalls;
    public RuntimeException failure;

    public SetView(Node<K, V> entry, int count) {
        this.entry = entry;
        this.count = count;
    }

    public Iterator<Map.Entry<K, V>> iterator() {
        iteratorCalls++;
        if (failure != null) throw failure;
        return new Cursor<Map.Entry<K, V>>(entry);
    }

    public int size() {
        sizeCalls++;
        if (failure != null) throw failure;
        return count;
    }
}
