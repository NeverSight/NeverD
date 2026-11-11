package fixture;
import java.util.function.Function;
import java.util.function.BinaryOperator;
import java.util.stream.Collector;
public final class Factory {
    private Factory() {}
    @SuppressWarnings("unchecked")
    public static <T, U> Collector<T, ?, U> reduce(U seed, Function<? super T, ? extends U> mapper, BinaryOperator<U> operator) {
        Trace.targets++;
        Trace.seed = seed;
        Trace.mapper = mapper;
        Trace.operator = operator;
        Trace.input = Trace.source;
        Trace.hit(1);
        Trace.mapped = mapper.apply((T)Trace.input);
        Trace.hit(2);
        Trace.folded = operator.apply(seed, (U)Trace.mapped);
        Trace.hit(3);
        Trace.action.run();
        Trace.hit(4);
        return (Collector<T, ?, U>)Trace.result;
    }
}
