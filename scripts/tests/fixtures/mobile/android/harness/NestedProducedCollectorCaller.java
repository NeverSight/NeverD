import fixture.Core;
import fixture.Factory;
import java.util.function.Supplier;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class NestedProducedCollectorCaller {
    public static Object call(int route, Supplier supplier) {
        if (route == 0) return Core.returned(supplier);
        if (route == 1) return Core.ignored(supplier);
        if (route == 2) return Core.fromArray(new Supplier[]{null, supplier});
        return Factory.make(supplier);
    }
}
