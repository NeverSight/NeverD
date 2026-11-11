package fixture;
public final class Effects {
    public static int unboxCalls, targetCalls, primitiveCalls;
    public static long lastInput, lastLeft, lastRight;
    public static Object lastSelf, lastUnboxed;
    public static RuntimeException unboxFailure, targetFailure, primitiveFailure;
}
