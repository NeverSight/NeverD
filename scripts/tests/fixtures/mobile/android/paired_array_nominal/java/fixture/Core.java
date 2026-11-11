package fixture;
public final class Core {
    public Core() {}
    @SuppressWarnings("unchecked")
    public static Result call(Object value, Property property, Evaluator evaluator, Object[] values) {
        return Provider.make(value, property, evaluator, values);
    }
    @SuppressWarnings("unchecked")
    public static void discard(Object value, Property property, Evaluator evaluator, Object[] values) {
        Provider.make(value, property, evaluator, values);
    }
}
