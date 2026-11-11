package fixture;
import java.util.function.BinaryOperator;
import java.util.stream.Collector;
import java.util.stream.Collectors;
public final class Core {
    private Core() {}
    public static Bucket<?,?,?> raw(Object seed, BinaryOperator<Object> operator) { return Factory.reduce(seed,operator); }
    public static Object ignored(Object seed, BinaryOperator<Object> operator) { Factory.reduce(seed,operator); return Trace.marker; }
    public static <T> Bucket<T,?,T> typed(T seed, BinaryOperator<T> operator) { return Factory.reduce(seed,operator); }
    public static Bucket<?,?,?> alias(Object seed, BinaryOperator<Object> operator) { return Facade.reduce(seed,operator); }
    public static Bucket<?,?,?> reverse(Object seed, BinaryOperator<Object> operator) { return Factory.reverse(operator,seed); }
    public static <T> Collector<T,?,T> sdk(T seed, BinaryOperator<T> operator) { return Collectors.reducing(seed,operator); }
}
