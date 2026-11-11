package fixture;
import java.util.Map;
import java.util.AbstractMap;
public class Provider {
    public static <K,V> Map.Entry<K,V> forward(K key,V value){return make(key,value,false);}
    @SuppressWarnings("unchecked")
    public static <K,V> Map.Entry<K,V> make(K key,V value,boolean immutable){Trace.calls++;Trace.key=key;Trace.value=value;Trace.flag=immutable;Trace.hit(1);Map.Entry<K,V> created=immutable?new AbstractMap.SimpleImmutableEntry<K,V>(key,value):new AbstractMap.SimpleEntry<K,V>(key,value);Trace.created=created;Trace.hit(2);Trace.action.run();Trace.hit(3);return (Map.Entry<K,V>)Trace.result;}
}
