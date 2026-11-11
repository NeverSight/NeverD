import fixture.Core;
import fixture.Factory;
import java.util.function.Function;
import java.util.function.BinaryOperator;
import java.util.function.Supplier;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class MergingCollectorCaller {
    public static Object call(int route, Function first, Function second, BinaryOperator merger, Supplier supplier) {
        if (route == 0) return Core.returned(first, second, merger, supplier);
        if (route == 1) return Core.ignored(first, second, merger, supplier);
        if (route == 2) return Factory.merge(first, second, merger, supplier);
        return Core.standard(first, second, merger, supplier);
    }
}
