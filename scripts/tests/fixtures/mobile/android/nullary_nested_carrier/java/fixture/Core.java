package fixture;

public final class Core {
    private Core() {}

    public static Object raw() {
        return Provider.make();
    }

    public static String narrow() {
        return (String) Provider.make().token;
    }

    public static void ignore() {
        Provider.make();
    }

    public static <V> Carrier<V, ?, Box<V>> relay() {
        return Provider.make();
    }

    public static Object setFactory() {
        return java.util.stream.Collectors.toSet();
    }

    public static Object listFactory() {
        return java.util.stream.Collectors.toList();
    }
}
