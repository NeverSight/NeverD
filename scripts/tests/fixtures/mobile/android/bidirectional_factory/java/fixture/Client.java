package fixture;
public final class Client {
    @SuppressWarnings({"rawtypes","unchecked"})
    public static Carrier call(Callback forward,Callback reverse) { return Provider.create(forward,reverse); }
}
