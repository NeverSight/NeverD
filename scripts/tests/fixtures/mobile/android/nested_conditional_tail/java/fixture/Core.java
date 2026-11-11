package fixture;
public final class Core {
    public static Envelope make(Provider provider, long bits) {
        Object value;
        return new Envelope(new Item((value = provider.payload) == null ? provider.fallback() : value),
                provider.finish(bits), provider.extra);
    }
}
