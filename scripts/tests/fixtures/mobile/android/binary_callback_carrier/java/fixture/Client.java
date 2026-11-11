package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class Client {
    public static Carrier call(Carrier source, Comparison comparison) {
        return Provider.select(source, comparison);
    }
}
