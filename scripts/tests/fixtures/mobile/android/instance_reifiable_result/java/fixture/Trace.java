package fixture;

public final class Trace {
    private Trace() {}
    public static volatile Object value;
    public static Object marker, observedMarker;
    public static Provider receiver, specialReceiver, defaultReceiver;
    public static String name, key;
    public static Throwable caughtFailure;
    public static Runnable action;
    public static int calls, overrides, caught, stage, length;
}
