import fixture.*;
public final class InlinedAccessorNamesCaller {
    public static Core make(byte b, long j, double d) { return Client.make(b, j, d); }
    public static byte byteValue(Core c) { return Client.byteValue(c); }
    public static long longValue(Core c) { return Client.longValue(c); }
    public static double doubleValue(Core c) { return Client.doubleValue(c); }
    public static int compare(Api<Object> c, Object other) { return Client.compare(c, other); }
}
