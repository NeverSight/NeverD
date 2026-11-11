package fixture;
public class Trace {
    public static int events, calls, failAt, begin, end;
    public static Object self, token;
    public static Object[] reply, result;
    public static Runnable callback;
    public static RuntimeException failure;
    public static void hit(int step) {
        events = events * 10 + step;
        if (failAt == step) throw failure;
    }
}
