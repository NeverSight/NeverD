import fixture.Client;

public final class PairedSeedCaller {
    public static Object call(Object first, Object second) { return Client.call(first, second); }
    public static String checkedKey(Object first, Object second) { return Client.checkedKey(first, second); }
    public static String checkedValue(Object first, Object second) { return Client.checkedValue(first, second); }
}
