package fixture;
import java.util.*;
import java.util.function.Function;
public final class Core {
    private Core() {}
    public static <T,K,V> Map<K,V> flat(Collection<T> values,Function<T,K> first,Function<T,V> second) {return Factory.flat(values,first,second,false);}
    public static <T,K,V> Map<K,List<V>> list(Collection<T> values,Function<T,K> first,Function<T,V> second) {return Factory.list(values,first,second,false);}
    public static <T,K,V> Map<K,Map<V,T>> nested(Collection<T> values,Function<T,K> first,Function<T,V> second) {return Factory.nested(values,first,second,false);}
    public static <T,K,V> Map<K,Map<V,List<T>>> nestedList(Collection<T> values,Function<T,K> first,Function<T,V> second) {return Factory.nestedList(values,first,second,false);}
    @SuppressWarnings("unchecked")
    public static HashMap checked(Collection values,Function first,Function second) {return (HashMap)Factory.flat(values,first,second,false);}
    @SuppressWarnings("unchecked")
    public static Object ignored(Collection values,Function first,Function second) {Factory.nested(values,first,second,false);return Trace.marker;}
}
