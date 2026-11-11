package fixture;
public final class Probe {
    public static int calls;
    public static Object last;
    public static RuntimeException failure;
    public static boolean forceTrue;
    public static boolean forceFalse;
    public static boolean accept(Object value) {
        calls++;
        last = value;
        if (failure != null) throw failure;
        if (forceFalse) return false;
        return forceTrue || value instanceof Element;
    }
}
