package fixture;
public final class Trace {
    public static int failAt,created,parents;
    public static long events;
    public static Object proxy,partial,left,right,result;
    public static Runnable proxyAction,action,compareAction;
    public static RuntimeException failure;
    public static void hit(int event) {events=events*10+event;if(event==failAt)throw failure;}
}
