package fixture;
public final class Client {
    private Client() {}
    public static Core make(byte[] b, short[] s, int[] i, long[] j) {
        Trace.hit(1);
        return new Core(b, s, i, j);
    }
    public static byte[] bytes(Core c) {
        Trace.hit(2);
        byte[] result = c.readBytes();
        Trace.hit(3);
        return result;
    }
    public static short[] shorts(Core c) {
        Trace.hit(2);
        short[] result = c.readShorts();
        Trace.hit(3);
        return result;
    }
    public static int[] ints(Core c) {
        Trace.hit(2);
        int[] result = c.readInts();
        Trace.hit(3);
        return result;
    }
    public static long[] longs(Core c) {
        Trace.hit(2);
        long[] result = c.readLongs();
        Trace.hit(3);
        return result;
    }
    public static boolean consume(Api<Object> c, Object value) {
        Trace.hit(4);
        boolean result = c.add(value);
        Trace.hit(5);
        return result;
    }
}
