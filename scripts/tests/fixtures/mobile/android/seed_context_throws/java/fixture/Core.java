package fixture;

public final class Core {
    private Core() {}
    public static Object raw(Object seed, String label, Object[] values) {
        return Provider.read(seed, label, values);
    }
    public static String narrow(Object seed, String label, Object[] values) {
        return (String)Provider.read(seed, label, values);
    }
    public static void ignore(Object seed, String label, Object[] values) {
        Provider.read(seed, label, values);
    }
    public static <V> V relay(V seed, String label, Object[] values) {
        return Provider.read(seed, label, values);
    }
    public static Object fixed(Object seed, String label, Object[] values) {
        return Provider.readFixed(seed, label, values);
    }
}
