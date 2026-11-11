package fixture;
public final class Core {
    public Core() {}
    private static <T> Carrier<T> change(Carrier<T> input, T seed, boolean add) {
        if (seed == null) return input;
        if (add) {
            Trace.hit(1, input, seed);
            Carrier<T> result = input;
            if (result == null) result = new Carrier<T>();
            if (!result.contains(seed)) result.add(seed);
            Trace.hit(2, result, seed);
            return result;
        } else {
            Trace.hit(7, input, seed);
            if (input != null) { input.remove(seed); if (input.empty()) input = null; }
            Trace.hit(8, input, seed);
            return input;
        }
    }
    @SuppressWarnings("unchecked")
    public static Carrier call(Carrier input, Object seed, boolean add) {
        return change(input, seed, add);
    }
    @SuppressWarnings("unchecked")
    public static void discard(Carrier input, Object seed, boolean add) {
        change(input, seed, add);
    }
}
