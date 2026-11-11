package fixture;
public final class Core {
    public static Target make() {
        return new Target(new Pair(Provider.left() + Provider.right(), Provider.left() - Provider.right(),
            Provider.left() * Provider.right(), Provider.left() + (long)Provider.shift(),
            Provider.left() - (long)Provider.shift(), Provider.left() * (long)Provider.shift()));
    }
}
