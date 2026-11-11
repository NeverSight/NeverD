package fixture;
public final class Trace {
    public static int mode;
    public static long effects;
    public static RuntimeException failure;
    public static Inner inner;
    public static Outer outer;
    public static Object fallbackValue;
    public static void hit(int event) {
        effects = effects * 10 + event;
        if (mode == event) throw failure;
    }
    public static boolean choose(boolean value) { hit(2); return value; }
    public static Object fallback() { hit(5); return fallbackValue; }
}
