package fixture;
public class Trace {
    public static int events;
    public static int subclassCalls;
    public static int defaultCalls;
    public static int parentDefaultCalls;
    public static RuntimeException failure;
    public static void hit(int value) { events = events * 10 + value; }
}
