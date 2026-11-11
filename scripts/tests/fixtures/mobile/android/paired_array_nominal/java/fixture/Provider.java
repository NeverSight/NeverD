package fixture;
public final class Provider {
    public Provider() {}
    public static <T, U> Result make(T value, Property<T, U> property, Evaluator<U> evaluator, U... values) {
        Trace.hit(1, value, property, evaluator, values);
        U from = property.read(value);
        U next = evaluator.apply(from, values[0]);
        values[0] = next;
        property.write(value, next);
        Result actual = Trace.result;
        Trace.hit(2, value, property, evaluator, values);
        return actual;
    }
}
