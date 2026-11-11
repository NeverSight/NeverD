import fixture.Core;
import fixture.Factory;
import java.util.function.Function;
import java.util.function.Supplier;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class DependentCollectorCaller {
    public static Object call(int route, Function first, Function second, Supplier collection, Supplier map) {
        if (route == 0) return Core.returned(first, second, collection, map);
        if (route == 1) return Core.ignored(first, second, collection, map);
        if (route == 2) return Core.reordered(map, second, collection, first);
        return Factory.assemble(first, second, collection, map);
    }
}
