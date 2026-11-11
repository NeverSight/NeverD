package fixture;
public final class Core {
    public Core() {}
    public static <T> Result provide(T value, Property<T, Token> property, Context context) {
        Trace.hit(1, value, property, context, null);
        Token token = property.apply(value, context);
        context.state++;
        context.seen = value;
        Result actual = Trace.result;
        Trace.hit(2, value, property, context, token);
        return actual;
    }
    @SuppressWarnings("unchecked")
    public static Result call(Object value, Property property, Context context) {
        return Bridge.Factory.make(value, property, context);
    }
    @SuppressWarnings("unchecked")
    public static void discard(Object value, Property property, Context context) {
        Bridge.Factory.make(value, property, context);
    }
}
