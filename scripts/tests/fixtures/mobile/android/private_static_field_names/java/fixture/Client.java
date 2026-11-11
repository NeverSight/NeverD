package fixture;
public final class Client {
    public static Callback<Object,Object> call(Callback<Object,Object> next,long bits) { return Provider.exchange(next,bits); }
}
