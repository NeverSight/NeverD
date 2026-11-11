package fixture;
public final class Trace {
    public static Probe probe;
    public static Object seen, replacement, after;
    public static int length;
    public static boolean write;
    public static Receipt result, alternate;
    public static void hit(int phase, Object value) {
        if (probe != null) probe.hit(phase, value);
    }
}
