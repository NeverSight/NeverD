package fixture;
import java.util.Map;
import java.util.LinkedHashMap;
public final class Provider {
    @SuppressWarnings("unchecked")
    public static <K,V> Map<K,V> merge(Map<? extends K,? extends V> first,Map<? extends K,? extends V> second) {
        Trace.hit(1);Trace.first=first;Trace.hit(2);Trace.second=second;Trace.hit(3);
        Map<K,V> result=new LinkedHashMap<K,V>();Trace.created=result;Trace.hit(4);
        result.putAll(first);Trace.hit(5);result.putAll(second);Trace.hit(6);
        if(Trace.kind==1)return (Map<K,V>)Trace.other;
        if(Trace.kind==2)return null;
        if(Trace.kind==3)return (Map<K,V>)first;
        return result;
    }
}
