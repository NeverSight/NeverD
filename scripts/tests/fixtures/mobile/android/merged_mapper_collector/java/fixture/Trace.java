package fixture;

public final class Trace {
    private Trace() {}
    public static int targets, stage;
    public static boolean invoke;
    public static Object source, seen, left, right, result;
    public static java.util.function.Function first, second;
    public static java.util.function.BinaryOperator operator;
    public static Object extra,merged;
    public static Runnable action;
}
