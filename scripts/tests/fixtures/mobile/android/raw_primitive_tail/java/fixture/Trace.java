package fixture;
public final class Trace {
    public static int targets, callbacks, unboxCalls;
    public static long value;
    public static Object result, seen;
    public static Token token;
    public static Sink<?> tail;
    public static RuntimeException targetFailure;
}
