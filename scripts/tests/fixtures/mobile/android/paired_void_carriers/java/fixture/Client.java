package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class Client {
    public static void call(Provider provider, int flags, Pair pair, Object input, Object token, long bits) {
        provider.accept(flags, pair, input, token, bits);
        Trace.hit(7);
    }
}
