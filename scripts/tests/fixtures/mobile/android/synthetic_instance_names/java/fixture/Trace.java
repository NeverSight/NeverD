package fixture;
public class Trace {
    public static int events, failAt;
    public static RuntimeException failure;
    public static Object value;
    public static Core receiver;
    public static void hit(int stage) {
        events = events * 10 + stage;
        if (stage == failAt) throw failure;
    }
}
