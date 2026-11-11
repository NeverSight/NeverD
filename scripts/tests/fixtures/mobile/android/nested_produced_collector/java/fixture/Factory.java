package fixture;
import java.util.List;
import java.util.Map;
import java.util.function.Supplier;
import java.util.stream.Collector;
public final class Factory {
    private Factory() {}
    @SuppressWarnings("unchecked")
    public static <K, V, M extends Map<K, List<V>>> Collector<Map<K, V>, ?, M> make(Supplier<M> supplier) {
        Trace.targets++;
        Trace.supplier = supplier;
        Trace.hit(1);
        Trace.seed = supplier.get();
        Trace.hit(2);
        Trace.action.run();
        Trace.hit(3);
        return (Collector<Map<K, V>, ?, M>)Trace.result;
    }
}
