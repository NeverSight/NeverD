package fixture;
public final class Trace {
    public static long value;
    public static int failAt;
    public static RuntimeException failure;
    public static Object replacement, captureAt;
    public static int markerAt;
    public static Child seen;
    public static void step(int code) {
        value = value * 17 + code;
        if (code == failAt) throw failure;
    }
    public static void observe(Object object) {
        Child child = (Child) object;
        seen = child; captureAt = child.capture; markerAt = child.marker;
        step(6);
    }
}
