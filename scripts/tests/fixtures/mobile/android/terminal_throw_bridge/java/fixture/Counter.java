package fixture;
import java.util.Iterator;
@SuppressWarnings("rawtypes")
public final class Counter<K,V> implements CountView {
    public Iterator<Element<K,V>> value;
    public int calls;
    public RuntimeException failure;
    public Counter() {}
    public boolean add(Object value) {
        throw new UnsupportedOperationException("owned immutable operation");
    }
    public boolean add(Element<? extends K,? extends V> value) {
        throw new UnsupportedOperationException("owned immutable operation");
    }
    public Iterator<Element<K,V>> iterator() {
        calls++;
        if (failure != null) throw failure;
        return value;
    }
}
