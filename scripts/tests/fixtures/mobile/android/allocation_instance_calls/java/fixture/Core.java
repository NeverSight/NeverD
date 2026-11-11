package fixture;
public final class Core {
    public static Payload make(Provider provider, Object input) {
        return new Payload(provider.read(input), provider);
    }
}
