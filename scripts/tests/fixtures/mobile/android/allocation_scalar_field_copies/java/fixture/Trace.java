package fixture;
public final class Trace {
    public static int value, initMode, floatBits;
    public static long tail, doubleBits;
    public static RuntimeException failure;
    public static boolean failBody, afterFlag, afterWritten;
    public static Provider owner;
    public static Target seen;
    public static void step(int code) {
        value = value * 10 + code;
        if (code == initMode) throw failure;
    }
}
