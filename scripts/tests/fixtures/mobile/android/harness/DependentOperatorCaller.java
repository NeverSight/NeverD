import fixture.Core;
import fixture.Factory;
import java.util.function.BinaryOperator;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class DependentOperatorCaller {
    public static Object call(int route, BinaryOperator operator) {
        if (route == 0) return Core.returned(operator);
        if (route == 1) return Core.ignored(operator);
        if (route == 2) return Core.fromArray(new BinaryOperator[]{null, operator});
        return Factory.adapt(operator);
    }
}
