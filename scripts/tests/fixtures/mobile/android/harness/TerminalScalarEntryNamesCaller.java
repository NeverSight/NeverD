import fixture.*;
public final class TerminalScalarEntryNamesCaller {
    public static Core make() { return Client.make(); }
    public static boolean bytes(Core c, byte value) { return Client.bytes(c, value); }
    public static long longs(Core c, long value) { return Client.longs(c, value); }
    public static double doubles(Core c, double value) { return Client.doubles(c, value); }
    public static boolean reject(Core c, int value) { return Client.reject(c, value); }
    public static boolean consume(Api<Object> c, Object value) { return Client.consume(c, value); }
}
