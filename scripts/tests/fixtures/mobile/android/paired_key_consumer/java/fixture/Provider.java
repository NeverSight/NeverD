package fixture;
import java.util.Map;
import java.util.Comparator;
import java.util.TreeMap;
public class Provider {
    public static <K,V> TreeMap<K,V> forward(Map<K,V> source){return adapt(source,null);}
    @SuppressWarnings("unchecked")
    public static <K,V> TreeMap<K,V> adapt(Map<K,V> source,Comparator<? super K> comparator){Trace.calls++;Trace.source=source;Trace.comparator=comparator;Trace.hit(1);int order=comparator==null?0:comparator.compare((K)Trace.first,(K)Trace.second);Trace.order=order;Trace.hit(2);source.put((K)Trace.first,(V)Trace.inserted);Trace.hit(3);Trace.action.run();Trace.hit(4);return (TreeMap<K,V>)Trace.result;}
}
