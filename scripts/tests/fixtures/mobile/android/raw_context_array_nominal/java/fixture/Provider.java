package fixture;
public final class Provider {
    public Provider() {}
    public static <U> Result make(Property property, Evaluator<U> evaluator, U... values) {
        Trace.hit(1, property, evaluator, values);
        U from = (U)property.read(null);
        U next = evaluator.apply(from, values[0]);
        values[0] = next;
        property.write(null, next);
        Result actual = Trace.result;
        Trace.hit(2, property, evaluator, values);
        return actual;
    }
}
