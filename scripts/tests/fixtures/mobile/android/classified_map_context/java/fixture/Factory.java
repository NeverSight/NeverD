package fixture;
import java.util.*;
import java.util.function.Function;
public final class Factory {
    private Factory() {}
    @SuppressWarnings("unchecked")
    public static <E,K> Map<K,List<E>> group(Collection<E> values,Function<E,K> classifier,boolean flag) {
        Trace.calls++;
        Trace.values=values;
        Trace.classifier=classifier;
        Trace.flag=flag;
        Trace.kind=1;
        Trace.stage=1;
        Trace.action.run();
        Trace.stage=2;
        return (Map<K,List<E>>)Trace.result;
    }
    @SuppressWarnings("unchecked")
    public static <E,K> Map<K,E> value(Collection<E> values,Function<E,K> classifier,boolean flag) {
        Trace.calls++;
        Trace.values=values;
        Trace.classifier=classifier;
        Trace.flag=flag;
        Trace.kind=2;
        Trace.stage=1;
        Trace.action.run();
        Trace.stage=2;
        return (Map<K,E>)Trace.result;
    }
}
