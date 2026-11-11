package fixture;
public final class Trace {
    private Trace() {}
    public static Object input, seen, output, result;
    public static Callback callback;
    public static Runnable action;
    public static boolean invoke, failInitialization;
    public static RuntimeException initializationFailure;
    public static int calls, stage, initializations;
}
