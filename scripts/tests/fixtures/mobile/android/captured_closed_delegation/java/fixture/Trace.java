package fixture;
public final class Trace {
    public static long events;
    public static int failAt;
    public static RuntimeException failure;
    public static Object partial;
    public static Value value;
    public static void hit(int event) {
        events=events*10+event;
        if(failAt==event)throw failure;
    }
}
