package fixture;
public final class Trace {
    public static int value;
    public static int initMode;
    public static RuntimeException failure;
    public static boolean failBody;
    public static Object seen;
    public static void step(int code) {
        value = value * 10 + code;
        if (code == initMode) throw failure;
    }
}
