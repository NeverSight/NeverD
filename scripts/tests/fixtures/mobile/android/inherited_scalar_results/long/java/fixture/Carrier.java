package fixture;
public final class Carrier {
    public int calls;
    public long value;
    public RuntimeException failure;
    public Carrier(long value) { this.value = value; }
    public static int boxes;
    public static long lastBits;
    public static boolean returnNull, reuse;
    public static Carrier cached;
    public static RuntimeException boxFailure;
    public static Carrier box(long value) {
        boxes++; lastBits = value;
        if (boxFailure != null) throw boxFailure;
        if (returnNull) return null;
        if (reuse) return cached;
        return new Carrier(value);
    }
    public long readBits() { calls++; if (failure != null) throw failure; return value; }
}
