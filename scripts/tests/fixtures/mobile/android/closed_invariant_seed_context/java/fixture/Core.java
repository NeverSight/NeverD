package fixture;

public final class Core {
    private Core() {}
    public static Object raw(Context<String> map, Object seed, boolean flag, Config config) { return Provider.read(map, seed, flag, config); }
    public static String narrow(Context<String> map, Object seed, boolean flag, Config config) { return (String)Provider.read(map, seed, flag, config); }
    public static Object ignored(Context<String> map, Object seed, boolean flag, Config config) {
        Provider.read(map, seed, flag, config);
        return Trace.marker;
    }
    public static Object relay(Context<String> map, Object seed, Config config) { return Provider.read(map, seed, config); }
    public static String typed(Context<String> map, String seed, boolean flag, Config config) { return Provider.read(map, seed, flag, config); }
    public static Object[] array(Context<String> map, Object seed, boolean flag, Config config) { return Provider.array(map, seed, flag, config); }
    public static String[] typedArray(Context<String> map, String seed, boolean flag, Config config) { return Provider.array(map, seed, flag, config); }
}
