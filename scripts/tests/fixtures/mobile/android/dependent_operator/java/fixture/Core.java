package fixture;

import java.util.function.BinaryOperator;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class Core {
    private Core() {}
    public static BinaryOperator returned(BinaryOperator operator) {
        return Factory.adapt(operator);
    }
    public static Object ignored(BinaryOperator operator) {
        Factory.adapt(operator);
        return Trace.result;
    }
    public static BinaryOperator fromArray(BinaryOperator[] operators) {
        return Factory.adapt(operators[1]);
    }
}
