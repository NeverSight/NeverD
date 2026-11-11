package fixture;

public final class Core {
    private Core() {}

    public static Object raw(Runnable action, Class<?>[] tokens) {
        return Provider.read(action, tokens);
    }

    public static String narrow(Runnable action, Class<?>[] tokens) {
        return Provider.read(action, tokens);
    }

    public static void ignore(Runnable action, Class<?>[] tokens) {
        Provider.read(action, tokens);
    }

    public static <V> V relay(Runnable action, Class<?>[] tokens) {
        return Provider.read(action, tokens);
    }

    public static Object fixed(Runnable action, Class<?>[] tokens) {
        return Provider.fixed(Trace.context, action, Trace.argument, tokens);
    }
}
