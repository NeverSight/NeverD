package fixture;
public final class Client {
    private Client() {}
    public static Core make() { Trace.hit(1); return new Core(); }
    public static boolean bytes(Core c, byte value) { return c.byteEntry(value); }
    public static long longs(Core c, long value) { return c.longEntry(value); }
    public static double doubles(Core c, double value) { return c.doubleEntry(value); }
    public static boolean reject(Core c, int value) { return c.rejectInt(value); }
    public static boolean consume(Api<Object> c, Object value) { return c.add(value); }
}
