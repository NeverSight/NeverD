package fixture;
public final class Provider {
    static {
        Trace.initializations++;
        if (Trace.failInitialization) throw Trace.initializationFailure;
    }
    private Provider() {}
    @SuppressWarnings("unchecked")
    public static <T> Outcome resolve(Callback<T,?> callback) throws IllegalArgumentException {
        Trace.calls++;
        Trace.stage = 1;
        Trace.callback = callback;
        if (Trace.invoke) {
            T input = (T)Trace.input;
            Trace.seen = input;
            Trace.stage = 2;
            Trace.output = callback.apply(input);
        }
        Trace.stage = 3;
        Trace.action.run();
        Trace.stage = 4;
        return (Outcome)Trace.result;
    }
}
