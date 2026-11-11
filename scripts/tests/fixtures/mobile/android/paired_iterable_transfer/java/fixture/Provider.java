package fixture;
import java.util.Map;
@SuppressWarnings({"rawtypes", "unchecked"})
public class Provider {
    static { Trace.hit(1); }
    public static <K, V> Map<K, V> merge(Map<? extends K, ? extends V> first, Iterable<? extends Pair<? extends K, ? extends V>> rows) {
        Trace.firstCount = first.size();
        Map<K, V> output = (Map<K, V>)Trace.output;
        write(output, rows);
        Trace.hit(11);
        return (Map<K, V>)Trace.returned;
    }
    public static <K, V> void write(Map<? super K, ? super V> sink, Iterable<? extends Pair<? extends K, ? extends V>> rows) {
        Trace.hit(2);
        for (Pair<? extends K, ? extends V> pair : rows) {
            K key = pair.component1();
            V value = pair.component2();
            sink.put(key, value);
            Trace.hit(7);
        }
    }
}
