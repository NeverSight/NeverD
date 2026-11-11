package fixture;
import java.util.Map;
public final class Provider {
    @SuppressWarnings("unchecked")
    public static <K,V extends Comparable<? super V>> Map<K,V> transfer(Map<K,V> source,boolean reverse) {
        Trace.hit(1);
        Trace.input=source;
        Trace.reverse=reverse;
        Trace.action.run();
        Trace.hit(2);
        return (Map<K,V>)Trace.result;
    }
}
