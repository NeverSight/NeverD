package fixture;
import java.util.Iterator;
public final class View<K,V> implements Contract<Pair<? extends K,? extends V>> {
    public Iterator<Pair<K,V>> value;
    public int calls;
    public RuntimeException failure;
    public View() {}
    @SuppressWarnings({"unchecked", "rawtypes"})
    public Iterator<Pair<? extends K,? extends V>> values() {
        calls++;
        if (failure != null) throw failure;
        return (Iterator) value;
    }
}
