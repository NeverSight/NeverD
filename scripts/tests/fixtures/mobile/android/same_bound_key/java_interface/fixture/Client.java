package fixture;
public final class Client {
    public static Element call(Provider provider,Key key) { return provider.get(key); }
}
