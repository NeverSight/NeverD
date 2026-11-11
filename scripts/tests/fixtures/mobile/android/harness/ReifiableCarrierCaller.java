import fixture.Client;
public final class ReifiableCarrierCaller {
    public static Object call() { return Client.call(); }
    public static String checked() { return Client.checked(); }
}
