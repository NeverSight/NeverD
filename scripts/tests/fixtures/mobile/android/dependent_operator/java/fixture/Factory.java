package fixture;

import java.util.Map;
import java.util.function.BinaryOperator;

public final class Factory {
    private Factory() {}
    @SuppressWarnings("unchecked")
    public static <K, V, M extends Map<K, V>> BinaryOperator<M> adapt(BinaryOperator<V> operator) {
        Trace.targets++;
        Trace.operator = operator;
        Object first = Trace.left, second = Trace.right;
        Trace.first = first;
        Trace.second = second;
        Trace.hit(1);
        Trace.value = operator.apply((V)first, (V)second);
        Trace.hit(2);
        Trace.action.run();
        Trace.hit(3);
        return (BinaryOperator<M>)Trace.result;
    }
}
