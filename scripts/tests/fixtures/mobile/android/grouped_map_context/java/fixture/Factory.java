package fixture;
import java.util.Collection;
import java.util.Map;
import java.util.function.Function;
import java.util.stream.Collector;
public final class Factory {
    private Factory() {}
    @SuppressWarnings("unchecked")
    public static <E,K,D> Map<K,D> group(Collection<E> values,Function<E,K> classifier,Collector<E,?,D> downstream,boolean flag) {
        Trace.calls++;
        Trace.values=values;
        Trace.classifier=classifier;
        Trace.downstream=downstream;
        Trace.flag=flag;
        Trace.stage=1;
        Trace.action.run();
        Trace.stage=2;
        return (Map<K,D>)Trace.result;
    }
}
