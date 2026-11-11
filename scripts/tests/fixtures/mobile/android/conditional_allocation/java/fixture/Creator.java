package fixture;
public final class Creator {
    public Provider provider;
    public Target make() {
        return new Target(provider.app(), (Owner) provider.owner(),
                provider.current() != null ? provider.current().extras() : null);
    }
}
