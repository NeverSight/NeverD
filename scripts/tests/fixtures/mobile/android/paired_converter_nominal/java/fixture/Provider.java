package fixture;
public final class Provider {
    public Provider() {}
    public static <T, U> Result make(T value, Property<T, U> property, Converter<Token, U> converter, Context context) {
        Trace.hit(1, value, property, converter, context);
        U from = property.read(value);
        U next = converter.apply(context.point);
        context.seen = next;
        property.write(value, next);
        Result actual = Trace.result;
        Trace.hit(2, value, property, converter, context);
        return actual;
    }
}
