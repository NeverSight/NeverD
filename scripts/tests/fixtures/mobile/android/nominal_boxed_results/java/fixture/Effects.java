package fixture;
public final class Effects {
    public static int scalarCalls, boxCalls, events;
    public static long lastDelta, lastScalar, lastBoxInput;
    public static Core lastSelf, lastBox;
    public static boolean returnNull;
    public static RuntimeException scalarFailure, boxFailure;
}
