package fixture;
public final class Core {
    public Core() {}
    @SuppressWarnings("unchecked")
    public static Result call(Property property, Evaluator evaluator, Object[] values) {
        return Provider.make(property, evaluator, values);
    }
    @SuppressWarnings("unchecked")
    public static void discard(Property property, Evaluator evaluator, Object[] values) {
        Provider.make(property, evaluator, values);
    }
}
