package fixture;

public final class ArrayChoice {
    private ArrayChoice() {}

    private static <R> R choose(Object[] input, Fallback<? extends R> fallback) {
        if (input.length != 0) return (R) input;
        return fallback.get();
    }

    public static Object call(Object[] input, Fallback<?> fallback) {
        return choose(input, fallback);
    }
}
