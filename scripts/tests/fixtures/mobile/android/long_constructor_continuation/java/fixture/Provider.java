package fixture;

public class Provider {
    public Object seed;
    public Object tail;
    public Provider(Object seed, Object tail) {
        this.seed = seed;
        this.tail = tail;
    }
    public static Object seed(Provider provider) {
        Trace.hit(0, null, provider, -1);
        return provider.seed;
    }
    public static Object tail(Provider provider) {
        Trace.hit(5, null, provider, -1);
        return provider.tail;
    }
}
