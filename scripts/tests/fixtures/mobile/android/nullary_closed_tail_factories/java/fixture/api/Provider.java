package fixture.api;
import fixture.Trace;
import java.util.Map;
public class Provider {
    static { Trace.hit(1); }
    @SuppressWarnings("unchecked")
    public static <K> Map<K, Object> make() {
        Trace.hit(2);
        ((int[]) Trace.input)[0]++;
        Trace.hit(3);
        return (Map<K, Object>) Trace.result;
    }
}
