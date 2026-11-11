package fixture;

public class Trace {
    public static Trace hook;
    public Trace() {}
    public void observe(int phase, Object producer, Config config, long bits, Object value) {}
    public Factory<Leaf> provide(Base value, Config config, long bits) { return null; }

    public static void hit(int phase, Object producer, Config config, long bits, Object value) {
        if (hook != null) hook.observe(phase, producer, config, bits, value);
    }
    public static Factory<Leaf> finish(Base value, Config config, long bits) {
        return hook == null ? null : hook.provide(value, config, bits);
    }
    public static Context argument(Config config) {
        hit(6, null, config, 0L, null);
        return config == null ? null : config.input;
    }
}
