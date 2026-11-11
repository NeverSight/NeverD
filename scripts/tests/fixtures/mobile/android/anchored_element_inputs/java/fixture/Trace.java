package fixture;
public final class Trace {
    public static long events;
    public static int failAt;
    public static float offset,limit;
    public static boolean override;
    public static RuntimeException failure;
    public static Object cell,produced,mapped,selected,result;
    public static void hit(int event) {
        events=events*10+event;
        if(failAt==event)throw failure;
    }
}
