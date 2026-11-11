package fixture;
public class Trace {
    public static int events, failAt;
    public static RuntimeException failure;
    public static Object seen;
    public static Owner.Child child;
    public static void hit(int event) {
        events = events * 10 + event;
        if (event == failAt) throw failure;
    }
}
