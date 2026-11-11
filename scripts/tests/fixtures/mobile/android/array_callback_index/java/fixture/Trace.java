package fixture;
public class Trace {
    public static int calls,failAt,start,result,callbackCalls;
    public static long events;
    public static Object filter,payload,argument;
    public static Object[] values;
    public static Runnable action;
    public static RuntimeException failure;
    public static void hit(int event){events=events*10+event;if(failAt==event)throw failure;}
}
