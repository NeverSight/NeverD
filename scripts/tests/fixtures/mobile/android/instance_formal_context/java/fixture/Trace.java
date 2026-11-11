package fixture;
public class Trace {
    public static Provider provider;
    public static int calls,failAt;
    public static long events;
    public static Object receiver,type,value,fallback,result,payload;
    public static Runnable action;
    public static Failure failure;
    public static void hit(int event) {events=events*10+event;if(failAt==event)throw failure;}
}
