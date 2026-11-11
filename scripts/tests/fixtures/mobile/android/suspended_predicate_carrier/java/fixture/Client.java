package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class Client {
    public static Carrier pick(Carrier source, Callback callback) { return Facade.choose(source, callback); }
}
