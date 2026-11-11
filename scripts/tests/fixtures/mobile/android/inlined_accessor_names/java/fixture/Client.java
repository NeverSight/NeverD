package fixture;
public final class Client {
    private Client() {}
    public static Core make(byte small, long wide, double real) {
        Trace.hit(1);
        return new Core(small, wide, real);
    }
    public static byte byteValue(Core value) { return value.readByte(); }
    public static long longValue(Core value) { return value.readLong(); }
    public static double doubleValue(Core value) { return value.readDouble(); }
    public static int compare(Api<Object> receiver, Object value) {
        return receiver.compare(value);
    }
}
