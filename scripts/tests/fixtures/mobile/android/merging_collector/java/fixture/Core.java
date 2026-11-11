package fixture;
import java.util.function.Function;
import java.util.function.BinaryOperator;
import java.util.function.Supplier;
import java.util.stream.Collector;
import java.util.stream.Collectors;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class Core {
    private Core() {}
    public static Collector returned(Function first, Function second, BinaryOperator merger, Supplier supplier) {
        return Factory.merge(first, second, merger, supplier);
    }
    public static Object ignored(Function first, Function second, BinaryOperator merger, Supplier supplier) {
        Factory.merge(first, second, merger, supplier);
        return Trace.result;
    }
    public static Collector standard(Function first, Function second, BinaryOperator merger, Supplier supplier) {
        return Collectors.toMap(first, second, merger, supplier);
    }
}
