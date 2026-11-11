package fixture;

public final class Trace {
    private Trace() {}
    public static Object input, seenInput, key, result, firstInput, lastInput;
    public static Object comparedReceiver, comparedArgument;
    public static int initializations, calls, stage, callbacks, keyComparisons, outputComparisons, comparison;
    public static boolean failInitialization;
    public static RuntimeException initializationFailure;
}
