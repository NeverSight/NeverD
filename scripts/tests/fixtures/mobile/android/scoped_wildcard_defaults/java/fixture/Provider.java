package fixture;
import java.util.Map;
public interface Provider<K, V> extends Root<K, Entry<K, V>> {
    default void putAll(Map<? extends K, ? extends Entry<K, V>> entries) {
        Trace.calls++;
        Trace.source = entries;
        if (Trace.failure != null) throw Trace.failure;
        if (entries == null) return;
        for (Map.Entry<? extends K, ? extends Entry<K, V>> item : entries.entrySet()) {
            Trace.key = item.getKey();
            Entry<K, V> value = item.getValue();
            Trace.value = value;
            Trace.visits++;
        }
    }
}
