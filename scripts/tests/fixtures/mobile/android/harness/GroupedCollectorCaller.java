import fixture.Core;
import fixture.Factory;
import java.util.function.Function;
import java.util.function.Supplier;
import java.util.stream.Collector;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class GroupedCollectorCaller {
    public static Object call(int route, Function classifier, Supplier map, Collector downstream) {
        if (route == 0) return Core.returned(classifier, map, downstream);
        if (route == 1) return Core.ignored(classifier, map, downstream);
        if (route == 2) return Core.reordered(downstream, classifier, map);
        return Factory.group(classifier, map, downstream);
    }
}
