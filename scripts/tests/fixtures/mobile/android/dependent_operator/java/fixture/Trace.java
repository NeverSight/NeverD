package fixture;

import java.util.function.BinaryOperator;

public final class Trace {
    private Trace() {}
    public static int targets;
    public static long events;
    public static Object left, right, first, second, value, result;
    public static BinaryOperator operator;
    public static Runnable action;
    public static void hit(int digit) { events = events * 10 + digit; }
}
