package fixture;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class Core {
    private Core() {}
    public static Map directKey(Iterator input,Map target,Mapper key) {return Factory.byKey(input,target,key);}
    public static <K,V> Map<K,V> relayKey(Iterator<V> input,Map<K,V> target,Mapper<V,K> key) {return Factory.byKey(input,target,key);}
    public static Object ignoredKey(Iterator input,Map target,Mapper key) {Factory.byKey(input,target,key);return Trace.marker;}
    public static Map directBoth(Iterator input,Map target,Mapper key,Mapper value) {return Factory.byBoth(input,target,key,value);}
    public static <K,V,E> Map<K,V> relayBoth(Iterator<E> input,Map<K,V> target,Mapper<E,K> key,Mapper<E,V> value) {return Factory.byBoth(input,target,key,value);}
    public static Object ignoredBoth(Iterator input,Map target,Mapper key,Mapper value) {Factory.byBoth(input,target,key,value);return Trace.marker;}
}
