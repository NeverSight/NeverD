package fixture;

import java.util.function.Function;
import java.util.function.Supplier;
import java.util.stream.Collector;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class Core {
    private Core() {}
    public static Collector returned(Function first, Function second, Supplier collection, Supplier map) {
        return Factory.assemble(first, second, collection, map);
    }
    public static Object ignored(Function first, Function second, Supplier collection, Supplier map) {
        Factory.assemble(first, second, collection, map);
        return Trace.result;
    }
    public static Collector reordered(Supplier map, Function second, Supplier collection, Function first) {
        return Factory.assemble(first, second, collection, map);
    }
}
