package fixture;
public class Core {
    public static void call(Provider provider, Factory<?> factory) { provider.send(factory); }
}
