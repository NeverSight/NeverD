package fixture;

import java.util.Map;

public final class Core {
    private Core() {}
    public static Object raw(Map<?,?> map, Object seed, boolean flag, Config config) { return Provider.read(map, seed, flag, config); }
    public static String narrow(Map<?,?> map, Object seed, boolean flag, Config config) { return (String)Provider.read(map, seed, flag, config); }
    public static Object ignored(Map<?,?> map, Object seed, boolean flag, Config config) {
        Provider.read(map, seed, flag, config);
        return Trace.marker;
    }
    public static Object relay(Map<?,?> map, Object seed, Config config) { return Provider.read(map, seed, config); }
    public static String typed(Map<?,?> map, String seed, boolean flag, Config config) { return Provider.read(map, seed, flag, config); }
    public static Object[] array(Map<?,?> map, Object seed, boolean flag, Config config) { return Provider.array(map, seed, flag, config); }
    public static String[] typedArray(Map<?,?> map, String seed, boolean flag, Config config) { return Provider.array(map, seed, flag, config); }
}
