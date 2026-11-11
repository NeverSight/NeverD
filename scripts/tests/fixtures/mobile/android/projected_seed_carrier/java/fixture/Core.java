package fixture;
public class Core {
    public static void call(Object carrier, Object value, Callback<Object,Object,Object> callback) {
        Carrier.send(carrier, value, callback);
    }
}
