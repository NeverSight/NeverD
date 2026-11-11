import fixture.*;
public final class ScopedPairTransferCaller {
    public static Object call(Pair<?,?> pair) { return Client.call(pair); }
    public static String key(Pair<?,?> pair) { return Client.key(pair); }
    public static String value(Pair<?,?> pair) { return Client.value(pair); }
}
