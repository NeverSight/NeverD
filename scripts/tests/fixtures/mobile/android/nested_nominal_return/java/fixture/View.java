package fixture;
import java.util.Iterator;
import java.util.Map;
public final class View implements Iterable<Map.Entry<? extends Key<?>, ?>>, Marker {
    public Iterator<Map.Entry<Key<?>, Object>> value;
    public int calls;
    public RuntimeException failure;
    @SuppressWarnings({"unchecked", "rawtypes"})
    public Iterator<Map.Entry<? extends Key<?>, ?>> iterator() {
        calls++;
        if (failure != null) throw failure;
        return (Iterator) value;
    }
}
