package fixture;
import java.util.function.Function;
import java.util.Map;
public class Factory {
    public Factory() {}
    @SuppressWarnings("unchecked")
    public static <T,K,A,D> Bucket<T,?,Map<K,D>> group(Function<? super T,? extends K> mapper, Bucket<? super T,A,D> downstream) {
        Trace.calls++;
        Trace.mapper = mapper;
        Trace.downstream = downstream;
        Trace.stage = 1;
        Trace.action.run();
        Trace.stage = 2;
        return (Bucket<T,?,Map<K,D>>)Trace.value;
    }
    public static <T,K,A,D> Bucket<T,?,Map<K,D>> reverse(Bucket<? super T,A,D> downstream, Function<? super T,? extends K> mapper) {
        Trace.reverses++;
        return group(mapper,downstream);
    }
}
