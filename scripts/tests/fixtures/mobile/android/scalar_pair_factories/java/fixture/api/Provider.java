package fixture.api;
import fixture.Trace;
import java.util.Map;
class Provider {
    static { Trace.hit(1); }
    @SuppressWarnings("unchecked")
    public static <K, V> Map<K, V> make(int capacity) {
        Trace.hit(2);
        Trace.capacitySeen = capacity;
        ((int[]) Trace.input)[0]++;
        Trace.hit(3);
        return (Map<K, V>) Trace.result;
    }
}
