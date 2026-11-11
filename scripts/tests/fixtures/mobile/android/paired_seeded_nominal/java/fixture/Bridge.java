package fixture;
final class Bridge {
    Bridge() {}
    static final class Factory {
        Factory() {}
        @SuppressWarnings("unchecked")
        static <T, U> Result make(T value, Property<T, U> property, Context context) {
            return Core.provide(value, (Property)property, context);
        }
    }
}
