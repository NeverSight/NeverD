package fixture;

public final class Provider {
    private Provider() {}

    @SuppressWarnings("unchecked")
    public static <T> T read(String label, Runnable action, long stamp, Class<?>... tokens) {
        Trace.calls++;
        Trace.stage = 1;
        Trace.label = label;
        Trace.action = action;
        Trace.tokens = tokens;
        Trace.stamp = stamp;
        int length = tokens.length;
        Trace.stage = 2;
        Trace.first = tokens[0];
        Trace.length = length;
        Trace.stage = 3;
        action.run();
        Trace.stage = 4;
        return (T) Trace.value;
    }

    public static <R> R read(Runnable action, Class<?>... tokens) {
        Trace.relays++;
        return read(Trace.context, action, Trace.argument, tokens);
    }

    public static <U> U fixed(String label, Runnable action, long stamp, Class<?>[] tokens) {
        return read(label, action, stamp, tokens);
    }
}
