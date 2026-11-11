package fixture;

public final class Carrier<S extends Base<S>> {
    private static final Object CLOSED = new Object();
    private final Object value;
    public static int calls;
    public static int closedCalls;
    public static RuntimeException failure;
    private Carrier(Object value) { this.value = value; }
    public static final Carrier wrap(Object value) { return new Carrier(value); }
    public final Object unwrap() { return value; }
    public static Object closedSentinel() { return CLOSED; }
    public static final Base<?> segment(Object value) {
        ++calls;
        if (value == CLOSED) {
            ++closedCalls;
            throw new IllegalStateException();
        }
        if (failure != null) throw failure;
        if (value == null) throw new NullPointerException();
        Base<?> result = (Base<?>) value;
        return result;
    }
}
