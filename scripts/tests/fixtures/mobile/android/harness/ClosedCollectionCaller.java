import fixture.Client;
public final class ClosedCollectionCaller {
    public static Object call() { return Client.call(); }
    public static Object checked() { return Client.checked(); }
}
