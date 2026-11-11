package fixture;
import java.util.Map;
public final class Provider {
    @SuppressWarnings("unchecked")
    public static <K,V> Map<K,V> make(Pair<? extends K,? extends V> pair) {
        Trace.hit(1);Trace.pair=pair;
        Trace.first=pair.first();Trace.hit(2);
        Trace.second=pair.second();Trace.hit(3);
        return (Map<K,V>)Trace.result;
    }
}
