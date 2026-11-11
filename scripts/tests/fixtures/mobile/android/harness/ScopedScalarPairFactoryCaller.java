import fixture.*;
public final class ScopedScalarPairFactoryCaller {
    public static Object call(boolean flag) { return Client.call(flag); }
    public static String value(boolean flag) { return Client.value(flag); }
    public static String key(boolean flag) { return Client.key(flag); }
}
