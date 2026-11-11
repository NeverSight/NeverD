package fixture;

public final class Provider {
    private Provider() {}
    public static <T> T read(Context<String> map, T seed, Config config) {
        Trace.relays++;
        return Provider.read(map, seed, false, config);
    }
    @SuppressWarnings("unchecked")
    public static <T> T read(Context<String> map, T seed, boolean flag, Config config) {
        Trace.calls++;
        Trace.map = map;
        Trace.seed = seed;
        Trace.flag = flag;
        Trace.config = config;
        Trace.stage = 1;
        Trace.configMarker = config.marker;
        Trace.stage = 2;
        if (Trace.inspect) {
            Trace.size = map.size();
            Trace.stage = 3;
        }
        Trace.action.run();
        Trace.stage = 4;
        Trace.afterFlag = flag;
        return (T)Trace.value;
    }
    @SuppressWarnings("unchecked")
    public static <T> T[] array(Context<String> map, T seed, boolean flag, Config config) {
        Trace.calls++;
        Trace.map = map;
        Trace.seed = seed;
        Trace.flag = flag;
        Trace.config = config;
        Trace.stage = 1;
        Trace.configMarker = config.marker;
        Trace.stage = 2;
        if (Trace.inspect) {
            Trace.size = map.size();
            Trace.stage = 3;
        }
        Trace.action.run();
        Trace.stage = 4;
        Trace.afterFlag = flag;
        return (T[])Trace.value;
    }
}
