package fixture;
import java.util.Collection;
import java.util.function.Supplier;
import java.util.stream.Collector;
public final class Factory {
    private Factory() {}
    @SuppressWarnings("unchecked")
    public static <T, C extends Collection<T>> Collector<T, ?, C> make(Supplier<C> supplier) {
        Trace.targets++;
        Trace.supplier = supplier;
        Trace.hit(1);
        Trace.collection = supplier.get();
        Trace.hit(2);
        Trace.action.run();
        Trace.hit(3);
        return (Collector<T, ?, C>)Trace.result;
    }
}
