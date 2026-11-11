package fixture;

public class Trace {
    public static int events;
    public static boolean failInit, failConstructor, failRecord, failInner, failOuter;
    public static RuntimeException constructorFailure, handlerFailure, cleanupFailure;
    public static Throwable last;
    public static Object tag;
    public static Problem published;

    public static void hit(int event) {
        events = events * 10 + event;
    }

    public static void record(Throwable failure, Object input) {
        hit(4);
        if (failRecord) throw handlerFailure;
        last = failure;
        tag = input;
    }

    public static void cleanup(int event) {
        hit(event);
        if ((event == 1 && failInner) || (event == 2 && failOuter)) throw cleanupFailure;
    }
}
