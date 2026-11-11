package fixture;
public final class Maker<T> {
    public static Object observed(Object input) {
        try {
            return new Payload(switch (0) { default -> { Trace.prepare(); yield input; } });
        } catch (Throwable failure) { return failure; }
    }
    public static Object fallback() {
        try {
            return new Payload(switch (0) { default -> { Trace.prepare(); yield Trace.token; } });
        } catch (Throwable failure) { return Fallback.value; }
    }
}
