package fixture;
public final class Core {
    public static Target make() {
        return new Target(new Pair(Provider.left() / Provider.right(), Provider.left() % Provider.right(),
            Provider.left() / (long)Provider.divisor(), Provider.left() % (long)Provider.divisor(),
            (Provider.left() / Provider.right()) * 0L, (Provider.left() % Provider.right()) * 0L));
    }
    public static Target caught() {
        try {
            return new Target(new Pair(Provider.left() / Provider.right(), Provider.left() % Provider.right(),
            Provider.left() / (long)Provider.divisor(), Provider.left() % (long)Provider.divisor(),
            (Provider.left() / Provider.right()) * 0L, (Provider.left() % Provider.right()) * 0L));
        } catch (ArithmeticException failure) {
            Trace.caught=failure;
            Trace.hit(9);
            throw failure;
        }
    }
}
