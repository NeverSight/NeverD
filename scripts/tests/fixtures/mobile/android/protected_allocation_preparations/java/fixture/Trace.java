package fixture;
public class Trace {
    public static int events, failAt;
    public static boolean failHandler;
    public static RuntimeException failure, handlerFailure;
    public static Throwable last;
    public static Object tag;
    public static Problem published;
    public static void hit(int event) {
        events = events * 10 + event;
        if (event == failAt) throw failure;
    }
    public static void logFailure(Throwable failure, Object input) {
        events = events * 10 + 4;
        if (failHandler) throw handlerFailure;
        last = failure; tag = input;
    }
}
