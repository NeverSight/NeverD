import fixture.Core;
import fixture.Factory;
import java.util.function.Supplier;
import java.util.stream.Collector;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class ProducedCollectorCaller {
    public static Object call(int route, Supplier supplier) {
        if (route == 0) return Core.returned(supplier);
        if (route == 1) return Core.ignored(supplier);
        return Factory.make(supplier);
    }
    public static Collector standard(Supplier supplier) { return Core.standard(supplier); }
}
