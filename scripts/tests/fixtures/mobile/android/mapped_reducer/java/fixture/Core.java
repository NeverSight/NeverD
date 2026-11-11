package fixture;
import java.util.function.Function;
import java.util.function.BinaryOperator;
import java.util.stream.Collector;
import java.util.stream.Collectors;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class Core {
    private Core() {}
    public static Collector returned(Object seed, Function mapper, BinaryOperator operator) { return Factory.reduce(seed, mapper, operator); }
    public static Object ignored(Object seed, Function mapper, BinaryOperator operator) { Factory.reduce(seed, mapper, operator); return Trace.result; }
    public static Collector standard(Object seed, Function mapper, BinaryOperator operator) { return Collectors.reducing(seed, mapper, operator); }
}
