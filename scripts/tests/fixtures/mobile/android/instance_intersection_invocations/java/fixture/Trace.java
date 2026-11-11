package fixture;
public class Trace {
    public static int events, failAt, firstCalls, secondCalls;
    public static Core core;
    public static Value replacement;
    public static Object receiver, argument;
    public static RuntimeException failure;
    public static void hit(int event) {
        events = events * 10 + event;
        if (event == 1) core.value = replacement;
        if (event == failAt) throw failure;
    }
}
