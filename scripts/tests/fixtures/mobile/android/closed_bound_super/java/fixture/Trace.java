package fixture;
public class Trace {
    public static int parents,calls,failAt;
    public static long events;
    public static Object partial,result;
    public static Runnable action,snapshotAction;
    public static RuntimeException failure;
    public static void hit(int event) {events=events*10+event;if(failAt==event)throw failure;}
}
