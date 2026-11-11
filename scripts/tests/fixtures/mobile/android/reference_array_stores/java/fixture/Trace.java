package fixture;
public final class Trace {
    public static int fail;
    public static int reads;
    public static long effects;
    public static Object[] array;
    public static Object seen;
    public static Target target;
    public static RuntimeException failure;
    public static void hit(int event) {
        effects = effects * 10 + event;
        if (fail == event) throw failure;
    }
    public static int size(int value) { hit(2); return value; }
    public static Object read(Object[] array, Object value) {
        Trace.array = array; seen = value; ++reads; hit(3); return value;
    }
}
