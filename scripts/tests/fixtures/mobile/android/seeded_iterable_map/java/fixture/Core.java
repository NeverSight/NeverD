package fixture;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class Core {
    private Core() {}
    public static Map directKey(Iterable input,Map target,Mapper key) {return Factory.byKey(target,input,key);}
    public static <K,V> Map<K,List<V>> relayKey(Iterable<V> input,Map<K,List<V>> target,Mapper<V,K> key) {return Factory.byKey(target,input,key);}
    public static Object ignoredKey(Iterable input,Map target,Mapper key) {Factory.byKey(target,input,key);return Trace.marker;}
    public static Map directBoth(Iterable input,Map target,Mapper key,Mapper value) {return Factory.byBoth(target,input,key,value);}
    public static <K,V,E> Map<K,List<V>> relayBoth(Iterable<E> input,Map<K,List<V>> target,Mapper<E,K> key,Mapper<E,V> value) {return Factory.byBoth(target,input,key,value);}
    public static Object ignoredBoth(Iterable input,Map target,Mapper key,Mapper value) {Factory.byBoth(target,input,key,value);return Trace.marker;}
}
