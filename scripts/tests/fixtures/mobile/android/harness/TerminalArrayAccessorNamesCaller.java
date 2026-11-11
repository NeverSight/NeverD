import fixture.*;
public final class TerminalArrayAccessorNamesCaller {
    public static Core make(byte[] b, short[] s, int[] i, long[] j) { return Client.make(b, s, i, j); }
    public static byte[] bytes(Core c) { return Client.bytes(c); }
    public static short[] shorts(Core c) { return Client.shorts(c); }
    public static int[] ints(Core c) { return Client.ints(c); }
    public static long[] longs(Core c) { return Client.longs(c); }
    public static boolean consume(Api<Object> c, Object value) { return Client.consume(c, value); }
}
