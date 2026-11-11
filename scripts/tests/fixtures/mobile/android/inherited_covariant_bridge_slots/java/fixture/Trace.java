package fixture;
public final class Trace {
    public static int steps;
    public static int derivedCalls;
    public static RuntimeException failure;
    public static void hit(int step) { steps = steps * 10 + step; }
}
