package fixture;

public final class Trace {
    private Trace() {}
    public static int targets, forwards, stage, index;
    public static boolean inspect, write, answer;
    public static Object[] left, right;
    public static Object value, seed, leftValue, rightValue, marker;
    public static Runnable action;
}
