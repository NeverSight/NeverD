import fixture.Client;

public final class NullaryPairCaller {
    public static Object call() { return Client.call(); }
    public static String checkedKey() { return Client.checkedKey(); }
    public static String checkedValue() { return Client.checkedValue(); }
}
