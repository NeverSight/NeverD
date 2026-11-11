package fixture;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public class Factory {
    public static <K,V> Map<K,V> byKey(Iterator<V> input,Map<K,V> target,Mapper<V,K> key) {
        Trace.calls++;Trace.input=input;Trace.target=target;Trace.key=key;Trace.value=null;Trace.stage=1;
        Trace.action.run();Trace.stage=2;return (Map<K,V>)Trace.result;
    }
    public static <K,V,E> Map<K,V> byBoth(Iterator<E> input,Map<K,V> target,Mapper<E,K> key,Mapper<E,V> value) {
        Trace.calls++;Trace.input=input;Trace.target=target;Trace.key=key;Trace.value=value;Trace.stage=1;
        Trace.action.run();Trace.stage=2;return (Map<K,V>)Trace.result;
    }
}
