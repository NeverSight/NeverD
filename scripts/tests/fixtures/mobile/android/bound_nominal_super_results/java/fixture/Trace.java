package fixture;
public class Trace {
    public static long events;
    public static int calls, failAt, inFirst, inSecond;
    public static Integer first, second;
    public static Object partial;
    public static Runnable action;
    public static RuntimeException failure;
    public static void hit(int event) {
        events = events * 10 + event;
        if (event == failAt) throw failure;
    }
}
