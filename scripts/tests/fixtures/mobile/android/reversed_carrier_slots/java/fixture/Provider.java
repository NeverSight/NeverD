package fixture;
import java.util.Map;
public final class Provider {
    @SuppressWarnings("unchecked") public static <K,V> Map<V,K> reverse(Map<K,V> source) {
        Trace.hit(1);
        Trace.input=source;
        Trace.action.run();
        Trace.hit(2);
        return (Map<V,K>)Trace.result;
    }
}
