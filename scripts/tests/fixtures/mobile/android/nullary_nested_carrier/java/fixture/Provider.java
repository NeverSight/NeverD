package fixture;

public final class Provider {
    private Provider() {}

    static {
        Trace.initializations++;
        if (Trace.initFailure != null) throw Trace.initFailure;
    }

    @SuppressWarnings("unchecked")
    public static <T> Carrier<T, ?, Box<T>> make() {
        Trace.calls++;
        Trace.stage = 1;
        Trace.action.run();
        Trace.stage = 2;
        return (Carrier<T, ?, Box<T>>) Trace.value;
    }
}
