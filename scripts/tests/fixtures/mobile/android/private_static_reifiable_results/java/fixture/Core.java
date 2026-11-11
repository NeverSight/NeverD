package fixture;

public final class Core {
    private Core() {}

    @SuppressWarnings("unchecked")
    private static <T> T read(Action action, Class<?> token, long stamp) {
        Trace.calls++;
        Trace.stage = 1;
        Trace.token = token;
        Trace.action = action;
        Trace.stamp = stamp;
        Trace.name = token.getName();
        Trace.stage = 2;
        action.run();
        Trace.stage = 3;
        return (T) Trace.value;
    }

    private static <U> U readArray(Action action, long stamp, Class<?>... tokens) {
        Trace.array = tokens;
        return read(action, tokens[0], stamp);
    }

    public static Object raw(Action action, Class<?> token, long stamp) {
        return read(action, token, stamp);
    }

    public static String narrow(Action action, Class<?> token, long stamp) {
        return (String) read(action, token, stamp);
    }

    public static void ignore(Action action, Class<?> token, long stamp) {
        read(action, token, stamp);
    }

    public static <R> R relay(Action action, Class<?> token, long stamp) {
        return read(action, token, stamp);
    }

    public static Object rawArray(Action action, long stamp, Class<?>[] tokens) {
        return readArray(action, stamp, tokens);
    }

    public static String narrowArray(Action action, long stamp, Class<?>[] tokens) {
        return (String) readArray(action, stamp, tokens);
    }

    public static void ignoreArray(Action action, long stamp, Class<?>[] tokens) {
        readArray(action, stamp, tokens);
    }

    public static <R> R relayArray(Action action, long stamp, Class<?>[] tokens) {
        return readArray(action, stamp, tokens);
    }
}
