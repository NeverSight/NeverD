package fixture;
public final class Trace {
    public static long events, wide, next;
    public static Object token;
    public static Hook hook;
    public static RuntimeException failure;
    public static int failAt, calls;
    public static void hit(int step) { events = events * 10 + step; if(failAt == step) throw failure; }
    public static long read(long bits, Object incoming, Hook callback) {
        hit(1); wide=bits; token=incoming; hook=callback; hit(2);
        long result=callback.read(bits,incoming); hit(3); return result;
    }
}
