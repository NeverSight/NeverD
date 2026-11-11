package fixture;
public final class Trace {
    public static int events, calls, failAt;
    public static Object receiver, input, token;
    public static Probe observer;
    public static RuntimeException failure;
    public static void hit(int step) {
        events = events * 10 + step;
        if (failAt == step) throw failure;
    }
    public static <R> R echo(Api self, Class<R> type, R value) {
        hit(1); receiver = self; input = value; token = type; calls++;
        hit(2); observer.observe(value, type); hit(3); return value;
    }
    public static Carrier<Number, String> inspect(Api self, Carrier<Number, String> value) {
        hit(4); receiver = self; input = value; token = null; calls++;
        hit(2); observer.observe(value, null); hit(3); return value;
    }
}
