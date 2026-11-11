package fixture;

public final class Provider {
    private Provider() {}

    @SuppressWarnings("unchecked")
    public static <T> Box<T>[] read(Class<T> token) throws IllegalStateException {
        Trace.calls++;
        Trace.stage = 1;
        Trace.token = token;
        Trace.name = token.getName();
        Trace.stage = 2;
        Trace.action.run();
        Trace.stage = 3;
        return (Box<T>[]) Trace.array;
    }
}
