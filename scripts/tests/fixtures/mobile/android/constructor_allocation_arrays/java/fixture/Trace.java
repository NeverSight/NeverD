package fixture;
public final class Trace {
    public static Probe probe;
    public static Object array, parcel, core;
    public static boolean after;
    public static void hit(int phase, Object value) {
        if (probe != null) probe.hit(phase, value);
    }
}
