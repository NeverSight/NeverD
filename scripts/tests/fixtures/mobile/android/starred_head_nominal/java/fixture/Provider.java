package fixture;
public final class Provider {
    public Provider() {}
    @SuppressWarnings("unchecked")
    public static <U> Result make(Property<?, U> property, Converter<Token, U> converter, Context context) {
        Trace.hit(1, null, property, converter, context);
        U from = property.read(null);
        U next = converter == null ? (U)(Object)context.point : converter.apply(context.point);
        context.seen = next;
        property.write(null, next);
        Result actual = Trace.result;
        Trace.hit(2, null, property, converter, context);
        return actual;
    }
}
