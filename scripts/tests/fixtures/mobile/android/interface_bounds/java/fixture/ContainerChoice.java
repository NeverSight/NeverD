package fixture;

public final class ContainerChoice {
    private ContainerChoice() {}

    private static <R> R choose(Container<?> input, Fallback<? extends R> fallback) {
        if (!input.isEmpty()) return (R) input;
        return fallback.get();
    }

    public static Object call(Container<?> input, Fallback<?> fallback) {
        return choose(input, fallback);
    }
}
