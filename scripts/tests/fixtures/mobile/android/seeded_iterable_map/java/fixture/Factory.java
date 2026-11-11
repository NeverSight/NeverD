package fixture;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public class Factory {
    public static <K,V> Map<K,List<V>> byKey(Map<K,List<V>> target,Iterable<V> input,Mapper<V,K> key) {
        Trace.calls++;Trace.input=input;Trace.target=target;Trace.key=key;Trace.value=null;Trace.stage=1;
        Trace.action.run();Trace.stage=2;return (Map<K,List<V>>)Trace.result;
    }
    public static <K,V,E> Map<K,List<V>> byBoth(Map<K,List<V>> target,Iterable<E> input,Mapper<E,K> key,Mapper<E,V> value) {
        Trace.calls++;Trace.input=input;Trace.target=target;Trace.key=key;Trace.value=value;Trace.stage=1;
        Trace.action.run();Trace.stage=2;return (Map<K,List<V>>)Trace.result;
    }
}
