package fixture;
public class Factory {
    public static int events;
    public Class savedToken;
    public Carrier<?> replacement;
    public RuntimeException failure;
    @SuppressWarnings("unchecked")
    public final <T extends Carrier<?>> T get(Class<T> token) {
        events = events * 5 + 1;
        savedToken = token;
        if (failure != null) throw failure;
        return (T) replacement;
    }
}
