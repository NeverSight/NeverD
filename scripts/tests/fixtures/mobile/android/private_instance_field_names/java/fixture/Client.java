package fixture;
public final class Client {
    public static Callback<Object,Object> call(Provider<Object> receiver, Callback<Object,Object> next, long bits) {
        return receiver.exchange(next, bits);
    }
}
