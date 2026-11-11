package fixture;
@SuppressWarnings({"rawtypes","unchecked"})
public final class Client {
    public static Box call(Box seed,Callback selector) { return Facade.choose(seed,selector); }
}
