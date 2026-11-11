package fixture;
public final class Trace {
    public static fixture.api.Facade receiver, alternativeReceiver;
    public static Object seenReceiver;
    public static int events, failAt;
    public static RuntimeException failure;
    public static Object input, result, after;
    public static Carrier<?> box, seen, alternative;
    public static void hit(int event) {
        events = events * 10 + event;
        if (event == 1 || event == 2) { box = alternative; receiver = alternativeReceiver; }
        if (failAt == event) throw failure;
    }
}
