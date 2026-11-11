package fixture;
public final class Creator {
    public Provider provider;
    public Target make() {
        return new Target(provider.index() < 0 ? provider.fallback : provider.values()[provider.index()]);
    }
}
