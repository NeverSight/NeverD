package fixture;
public class Trace {
    public static int parents,failAt;
    public static long events;
    public static Object partial,payload,result,comparison,left,right;
    public static Runnable action,functionAction,compareAction;
    public static RuntimeException failure;
    public static void hit(int event) {events=events*10+event;if(failAt==event)throw failure;}
}
