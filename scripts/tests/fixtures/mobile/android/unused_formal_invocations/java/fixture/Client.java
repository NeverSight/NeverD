package fixture;
public final class Client {
    public static Object call(Object input) {
        Object result = Provider.relay(input);
        Trace.after = result;
        return result;
    }
}
