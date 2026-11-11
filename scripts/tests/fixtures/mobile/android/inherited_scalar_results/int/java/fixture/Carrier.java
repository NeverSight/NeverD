package fixture;
public final class Carrier {
    public int calls;
    public int value;
    public RuntimeException failure;
    public Carrier(int value) { this.value = value; }
    public static int boxes;
    public static int lastBits;
    public static boolean returnNull, reuse;
    public static Carrier cached;
    public static RuntimeException boxFailure;
    public static Carrier box(int value) {
        boxes++; lastBits = value;
        if (boxFailure != null) throw boxFailure;
        if (returnNull) return null;
        if (reuse) return cached;
        return new Carrier(value);
    }
    public int readBits() { calls++; if (failure != null) throw failure; return value; }
}
