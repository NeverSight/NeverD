package fixture;

import java.util.function.Function;
import java.util.function.Supplier;
import java.util.stream.Collector;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class Core {
    private Core() {}
    public static Collector returned(Function classifier, Supplier map, Collector downstream) {
        return Factory.group(classifier, map, downstream);
    }
    public static Object ignored(Function classifier, Supplier map, Collector downstream) {
        Factory.group(classifier, map, downstream);
        return Trace.result;
    }
    public static Collector reordered(Collector downstream, Function classifier, Supplier map) {
        return Factory.group(classifier, map, downstream);
    }
}
