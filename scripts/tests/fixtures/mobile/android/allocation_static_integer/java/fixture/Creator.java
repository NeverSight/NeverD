package fixture;
public final class Creator {
    public Provider provider;
    public Target make() {
        return new Target(provider.context(), Fields.number);
    }
}
