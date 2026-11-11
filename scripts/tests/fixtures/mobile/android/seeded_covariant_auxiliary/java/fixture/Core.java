package fixture;

public final class Core {
    private Core() {}

    public static Object staticRaw(Object seed, Class<? extends Runnable> type) {
        return Provider.readStatic(seed, type);
    }

    public static String staticNarrow(Object seed, Class<? extends Runnable> type) {
        return (String) Provider.readStatic(seed, type);
    }

    public static void staticIgnore(Object seed, Class<? extends Runnable> type) {
        Provider.readStatic(seed, type);
    }

    public static <V> V staticRelay(V seed, Class<? extends Runnable> type) {
        return Provider.readStatic(seed, type);
    }

    public static Object virtualRaw(Provider receiver, Object seed, Class<? extends Runnable> type) {
        return receiver.read(seed, type);
    }

    public static String virtualNarrow(Provider receiver, Object seed, Class<? extends Runnable> type) {
        return (String) receiver.read(seed, type);
    }

    public static void virtualIgnore(Provider receiver, Object seed, Class<? extends Runnable> type) {
        receiver.read(seed, type);
    }

    public static <V> V virtualRelay(Provider receiver, V seed, Class<? extends Runnable> type) {
        return receiver.read(seed, type);
    }
}
