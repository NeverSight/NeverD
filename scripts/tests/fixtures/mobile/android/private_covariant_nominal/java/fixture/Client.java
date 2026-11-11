package fixture;
@SuppressWarnings("rawtypes")
public final class Client {
    public static String read(Provider provider, Producer producer, String context) {
        return provider.call(producer, context);
    }
}
