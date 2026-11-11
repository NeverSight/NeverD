package fixture;
import java.util.Map;
public final class Provider {
    @SuppressWarnings("unchecked")
    private static <K,V> Map<K,V> make(Pair<K,? extends V> pair) {
        Trace.hit(1);Trace.pair=pair;
        Trace.first=pair.first();Trace.hit(2);
        Trace.second=pair.second();Trace.hit(3);
        return (Map<K,V>)Trace.result;
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    public static Map call(Pair pair) { return make(pair); }
}
