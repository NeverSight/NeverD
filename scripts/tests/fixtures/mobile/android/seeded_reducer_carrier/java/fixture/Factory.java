package fixture;
import java.util.function.BinaryOperator;
public class Factory {
    public Factory() {}
    @SuppressWarnings("unchecked")
    public static <T> Bucket<T,?,T> reduce(T seed, BinaryOperator<T> operator) {
        Trace.calls++;
        Trace.seed = seed;
        Trace.operator = operator;
        Trace.stage = 1;
        Trace.action.run();
        Trace.stage = 2;
        return (Bucket<T,?,T>)Trace.value;
    }
    public static <T> Bucket<T,?,T> reverse(BinaryOperator<T> operator, T seed) {
        Trace.reverses++;
        return reduce(seed,operator);
    }
}
