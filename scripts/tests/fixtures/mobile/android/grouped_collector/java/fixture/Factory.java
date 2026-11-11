package fixture;

import java.util.Map;
import java.util.function.Function;
import java.util.function.Supplier;
import java.util.stream.Collector;

public final class Factory {
    private Factory() {}
    @SuppressWarnings("unchecked")
    public static <T, K, D, A, M extends Map<K, D>> Collector<T, ?, M> group(
            Function<? super T, ? extends K> classifier, Supplier<M> map,
            Collector<? super T, A, D> downstream) {
        Trace.targets++;
        Trace.classifier = classifier;
        Trace.mapSupplier = map;
        Trace.downstream = downstream;
        Object input = Trace.source;
        Trace.seen = input;
        Trace.hit(1);
        Trace.key = classifier.apply((T)input);
        Trace.hit(2);
        Trace.map = map.get();
        Trace.hit(3);
        Trace.state = downstream.supplier().get();
        Trace.hit(4);
        Trace.value = downstream.finisher().apply((A)Trace.state);
        Trace.hit(5);
        Trace.action.run();
        Trace.hit(6);
        return (Collector<T, ?, M>)Trace.result;
    }
}
