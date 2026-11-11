package fixture.api;
import fixture.Trace;
import java.util.Map;
class Provider {
    static { Trace.hit(1); }
    @SuppressWarnings("unchecked")
    public static <K, V> Map<K, V> make(Iterable<? extends Map<? extends K, ? extends V>> values) {
        Trace.first = values; Trace.hit(2);
        Map<?, ?> pair = values.iterator().next();
        Trace.second = pair; Trace.hit(3);
        ((int[]) pair.keySet().iterator().next())[0]++;
        Trace.hit(4);
        ((int[]) pair.get(Trace.lookup))[0] += 3;
        Trace.hit(5);
        return (Map<K, V>) Trace.result;
    }
}
