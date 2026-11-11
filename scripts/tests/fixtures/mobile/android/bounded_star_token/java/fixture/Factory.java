package fixture;
public class Factory {
    public static int events;
    public Class savedToken;
    public Probe probe;
    public Object marker;
    public Carrier<?> replacement, savedResult;
    public RuntimeException failure;
    @SuppressWarnings("unchecked")
    public final <T extends Carrier<?>> T get(Class<T> token) {
        events = events * 5 + 1;
        savedToken = token;
        marker = probe.observe(this, token);
        if (failure != null) throw failure;
        savedResult = replacement;
        events = events * 5 + 2;
        return (T) replacement;
    }
}
