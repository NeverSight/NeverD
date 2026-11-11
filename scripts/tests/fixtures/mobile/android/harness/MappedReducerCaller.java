import fixture.Core;
import fixture.Factory;
import java.util.function.Function;
import java.util.function.BinaryOperator;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class MappedReducerCaller {
    public static Object call(int route, Object seed, Function mapper, BinaryOperator operator) {
        if (route == 0) return Core.returned(seed, mapper, operator);
        if (route == 1) return Core.ignored(seed, mapper, operator);
        if (route == 2) return Factory.reduce(seed, mapper, operator);
        return Core.standard(seed, mapper, operator);
    }
}
