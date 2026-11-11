package fixture.api;
import fixture.Trace;
import java.util.Map;
class Provider {
    static { Trace.hit(1); }
    @SuppressWarnings("unchecked")
    public static <K, V> Map<K, V> make(K first, V second) {
        Trace.first = first; Trace.second = second; Trace.hit(2);
        ((int[]) first)[0]++;
        Trace.hit(3);
        ((int[]) second)[0] += 3;
        Trace.hit(4);
        return (Map<K, V>) Trace.result;
    }
}
