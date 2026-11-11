package fixture;

public final class Provider<T extends Number> {
    private Provider() {}

    @SuppressWarnings("unchecked")
    public static <T> boolean variable(T[] left, T... right) {
        Trace.targets++;
        Trace.left = left;
        Trace.right = right;
        Object saved = Trace.value;
        Trace.seed = saved;
        Trace.stage = 1;
        if (Trace.inspect) {
            Trace.leftValue = left[0];
            Trace.stage = 2;
            Trace.rightValue = right[0];
            Trace.stage = 3;
        }
        Trace.action.run();
        Trace.stage = 4;
        if (Trace.write) {
            left[Trace.index] = (T)saved;
            Trace.stage = 5;
        }
        return Trace.answer;
    }

    public static <T> boolean fixed(T[] left, T[] right) {
        Trace.forwards++;
        return Provider.variable(left, right);
    }
}
