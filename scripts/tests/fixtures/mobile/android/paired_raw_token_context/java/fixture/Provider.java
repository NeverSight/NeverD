package fixture;
import java.util.Map;
import java.util.HashMap;
public class Provider {
    @SuppressWarnings("unchecked")
    public static <K,V> Map<K,V> convert(Class<K> keys,Class<V> values,Object value) {
        return convert(value instanceof Map?(Class<? extends Map>)value.getClass():HashMap.class,keys,values,value);
    }
    @SuppressWarnings("unchecked")
    public static <K,V> Map<K,V> convert(Class<? extends Map> kind,Class<K> keys,Class<V> values,Object value) {
        Trace.calls++;Trace.kind=kind;Trace.keys=keys;Trace.values=values;Trace.value=value;Trace.hit(1);
        Trace.action.run();Trace.hit(2);return (Map<K,V>)Trace.result;
    }
}
