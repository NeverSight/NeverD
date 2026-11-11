package fixture;

public final class LambdaScope {
    private LambdaScope() {}

    public static <T> Duo<T, T> flip(Duo<T, T> input) {
        Mapper<Duo<T, T>> mapper = value -> new Duo<T, T>() {
            public T first() { return value.second(); }
            public T second() { return value.first(); }
            public T replace(T replacement) { return replacement; }
        };
        return mapper.map(input);
    }
}
