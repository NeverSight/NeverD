package fixture;
public final class Core {
    public Core() {}
    private static final class Factory {
        private Factory() {}
        static <T> Carrier<T> add(Carrier<T> input, T seed) {
            Trace.hit(1, input, seed);
            Carrier<T> result = input;
            if (result == null) result = new Carrier<T>();
            if (!result.contains(seed)) result.add(seed);
            Trace.hit(2, result, seed);
            return result;
        }
        static <T> Carrier<T> remove(Carrier<T> input, T seed) {
            Trace.hit(7, input, seed);
            if (input != null) { input.remove(seed); if (input.empty()) input = null; }
            Trace.hit(8, input, seed);
            return input;
        }
    }
    @SuppressWarnings("unchecked")
    public static Carrier call(Carrier input, Object seed, boolean add) {
        return add ? Factory.add(input, seed) : Factory.remove(input, seed);
    }
    @SuppressWarnings("unchecked")
    public static void discard(Carrier input, Object seed, boolean add) {
        if (add) Factory.add(input, seed); else Factory.remove(input, seed);
    }
}
