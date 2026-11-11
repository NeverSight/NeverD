package fixture;
public final class Provider {
    public static long left() { Trace.hit(1); return Trace.left; }
    public static long right() { Trace.hit(2); return Trace.right; }
    public static int shift() { Trace.hit(3); return Trace.shift; }
}
