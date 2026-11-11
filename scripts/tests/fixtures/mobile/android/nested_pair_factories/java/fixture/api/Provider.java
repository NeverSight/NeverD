package fixture.api;
import fixture.Trace;
import java.util.Map;
import java.util.List;
class Provider {
    static { Trace.hit(1); }
    @SuppressWarnings("unchecked")
    public static <K, V> List<Map<K, V>> make(Iterable<? extends K> first, Iterable<? extends V> second) {
        Trace.first = first; Trace.second = second; Trace.hit(2);
        ((int[]) first.iterator().next())[0]++;
        Trace.hit(3);
        ((int[]) second.iterator().next())[0] += 3;
        Trace.hit(4);
        return (List<Map<K, V>>) Trace.result;
    }
}
