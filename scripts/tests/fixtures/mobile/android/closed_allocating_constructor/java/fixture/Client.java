package fixture;
public final class Client {
    public static Core call(long bits,Callback<? super Value,? super Label,Value> callback) { return Core.create(bits,callback); }
}
