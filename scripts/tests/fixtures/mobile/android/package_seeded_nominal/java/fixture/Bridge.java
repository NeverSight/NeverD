package fixture;
final class Bridge {
    Bridge() {}
    static <T> Result make(T value, Property<T, Token> property, Context context) {
        return Core.provide(value, property, context);
    }
}
