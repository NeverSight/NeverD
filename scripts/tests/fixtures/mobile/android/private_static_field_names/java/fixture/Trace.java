package fixture;
public final class Trace {
    public static Callback<Object,Object> initial,seen;
    public static long initialBits,events;
    public static Object seenInput;
    public static int failAt;
    public static RuntimeException failure;
    public static void hit(int event) { events=events*10+event;if(failAt==event)throw failure; }
}
