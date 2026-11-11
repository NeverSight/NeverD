package fixture;
public class Factory {
    public static int events;
    public String savedKey;
    public Probe probe;
    public Object marker;
    public Carrier<?> replacement, savedResult;
    public RuntimeException failure;
    @SuppressWarnings("unchecked")
    public <T extends Carrier<?>> T get(String key) {
        events = events * 5 + 1;
        savedKey = key;
        marker = probe.observe(this, key);
        if (failure != null) throw failure;
        savedResult = replacement;
        events = events * 5 + 2;
        return (T) replacement;
    }
}
