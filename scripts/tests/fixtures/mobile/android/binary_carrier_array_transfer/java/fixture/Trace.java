package fixture;

public final class Trace {
    private Trace() {}
    public static Object input, value, store, saved, inputMarker, element, marker;
    public static Object[] values;
    public static Runnable action;
    public static boolean inspectInput, inspectArray, write;
    public static int calls, forwards, callbacks, stage, entry, index;
}
