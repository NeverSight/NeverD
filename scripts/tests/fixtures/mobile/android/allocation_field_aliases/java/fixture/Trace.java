package fixture;
public final class Trace {
    public static int value, initMode, floatBits;
    public static long tail, doubleBits;
    public static RuntimeException failure;
    public static boolean failBody, providerInit;
    public static Object payload, replacement, after, seen;
    public static void step(int code) {
        value = value * 10 + code;
        if (code == initMode) throw failure;
    }
}
