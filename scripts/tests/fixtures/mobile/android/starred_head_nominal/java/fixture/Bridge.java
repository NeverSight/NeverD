package fixture;
final class Bridge {
    Bridge() {}
    static final class Factory {
        Factory() {}
        static <U> Result make(Property<?, U> property, Context context) {
            return Provider.make(property, null, context);
        }
    }
}
