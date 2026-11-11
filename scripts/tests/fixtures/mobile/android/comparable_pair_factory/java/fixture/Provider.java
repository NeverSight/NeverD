package fixture;
import java.util.Map;
import java.util.Comparator;
public class Provider {
    @SuppressWarnings("unchecked")
    public static <K,V extends Comparable<? super V>> Comparator<Map.Entry<K,V>> select(){Trace.calls++;Trace.path=1;Trace.hit(1);Trace.action.run();Trace.hit(2);return (Comparator<Map.Entry<K,V>>)Trace.result;}
    public static <K,V extends Comparable<? super V>> Comparator<Map.Entry<K,V>> standard(){Trace.calls++;Trace.path=2;Trace.hit(1);Trace.action.run();Trace.hit(2);Comparator<Map.Entry<K,V>> actual=Map.Entry.comparingByValue();Trace.platform=actual;return actual;}
}
