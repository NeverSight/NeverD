package fixture;

import java.util.Collection;
import java.util.Map;
import java.util.function.Function;
import java.util.function.Supplier;
import java.util.stream.Collector;

public final class Factory {
    private Factory() {}

    @SuppressWarnings("unchecked")
    public static <T, K, R, C extends Collection<R>, M extends Map<K, C>>
            Collector<T, ?, M> assemble(Function<? super T, ? extends K> first,
            Function<? super T, ? extends R> second,
            Supplier<C> collection, Supplier<M> map) {
        Trace.targets++;
        Trace.first = first;
        Trace.second = second;
        Trace.collectionSupplier = collection;
        Trace.mapSupplier = map;
        Object value = Trace.source;
        Trace.seen = value;
        Trace.hit(1);
        Trace.left = first.apply((T)value);
        Trace.hit(2);
        Trace.right = second.apply((T)value);
        Trace.hit(3);
        Trace.collection = collection.get();
        Trace.hit(4);
        Trace.map = map.get();
        Trace.hit(5);
        Trace.action.run();
        Trace.hit(6);
        return (Collector<T, ?, M>)Trace.result;
    }
}
