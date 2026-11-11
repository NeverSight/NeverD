package fixture;
import java.util.Map;
import java.util.function.Function;
import java.util.function.BinaryOperator;
import java.util.function.Supplier;
import java.util.stream.Collector;
public final class Factory {
    private Factory() {}
    @SuppressWarnings("unchecked")
    public static <T, K, V, M extends Map<K, V>> Collector<T, ?, M> merge(
            Function<? super T, ? extends K> first, Function<? super T, ? extends V> second,
            BinaryOperator<V> merger, Supplier<M> supplier) {
        Trace.targets++;
        Trace.first = first;
        Trace.second = second;
        Trace.operator = merger;
        Trace.mapSupplier = supplier;
        Object value = Trace.source;
        Trace.seen = value;
        Trace.hit(1);
        Trace.left = first.apply((T)value);
        Trace.hit(2);
        Trace.right = second.apply((T)value);
        Trace.hit(3);
        Trace.merged = merger.apply((V)Trace.right, (V)Trace.extra);
        Trace.hit(4);
        Trace.map = supplier.get();
        Trace.hit(5);
        Trace.action.run();
        Trace.hit(6);
        return (Collector<T, ?, M>)Trace.result;
    }
}
