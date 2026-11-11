package fixture;
public final class Client {
    public static Item call(Api api,int index) { return api.get(index); }
}
