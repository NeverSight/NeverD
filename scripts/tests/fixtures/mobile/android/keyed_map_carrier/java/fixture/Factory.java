package fixture;
import java.util.*;
import java.util.function.Function;
public final class Factory {
    private Factory() {}
    @SuppressWarnings("unchecked")
    public static <T,K,V> Map<K,V> flat(Collection<T> values,Function<T,K> first,Function<T,V> second,boolean ordered) {
        Trace.targets++;
        Trace.values=values;
        Trace.first=first;
        Trace.second=second;
        Trace.ordered=ordered;
        Object value=Trace.source;
        Trace.seen=value;
        Trace.stage=1;
        if(Trace.invoke) {
            Trace.left=first.apply((T)value);
            Trace.stage=2;
            Trace.right=second.apply((T)value);
            Trace.stage=3;
        }
        Trace.action.run();
        Trace.stage=4;
        return (Map<K,V>)Trace.result;
    }
    @SuppressWarnings("unchecked")
    public static <T,K,V> Map<K,List<V>> list(Collection<T> values,Function<T,K> first,Function<T,V> second,boolean ordered) {
        Trace.targets++;
        Trace.values=values;
        Trace.first=first;
        Trace.second=second;
        Trace.ordered=ordered;
        Object value=Trace.source;
        Trace.seen=value;
        Trace.stage=1;
        if(Trace.invoke) {
            Trace.left=first.apply((T)value);
            Trace.stage=2;
            Trace.right=second.apply((T)value);
            Trace.stage=3;
        }
        Trace.action.run();
        Trace.stage=4;
        return (Map<K,List<V>>)Trace.result;
    }
    @SuppressWarnings("unchecked")
    public static <T,K,V> Map<K,Map<V,T>> nested(Collection<T> values,Function<T,K> first,Function<T,V> second,boolean ordered) {
        Trace.targets++;
        Trace.values=values;
        Trace.first=first;
        Trace.second=second;
        Trace.ordered=ordered;
        Object value=Trace.source;
        Trace.seen=value;
        Trace.stage=1;
        if(Trace.invoke) {
            Trace.left=first.apply((T)value);
            Trace.stage=2;
            Trace.right=second.apply((T)value);
            Trace.stage=3;
        }
        Trace.action.run();
        Trace.stage=4;
        return (Map<K,Map<V,T>>)Trace.result;
    }
    @SuppressWarnings("unchecked")
    public static <T,K,V> Map<K,Map<V,List<T>>> nestedList(Collection<T> values,Function<T,K> first,Function<T,V> second,boolean ordered) {
        Trace.targets++;
        Trace.values=values;
        Trace.first=first;
        Trace.second=second;
        Trace.ordered=ordered;
        Object value=Trace.source;
        Trace.seen=value;
        Trace.stage=1;
        if(Trace.invoke) {
            Trace.left=first.apply((T)value);
            Trace.stage=2;
            Trace.right=second.apply((T)value);
            Trace.stage=3;
        }
        Trace.action.run();
        Trace.stage=4;
        return (Map<K,Map<V,List<T>>>)Trace.result;
    }
}
