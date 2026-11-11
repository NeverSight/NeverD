package fixture;
public class Trace {
    public static int calls,failAt,dispatch,callbackCalls;
    public static long events;
    public static Object receiver,result,marker,payload;
    public static Handler<?> handler;
    public static java.io.BufferedReader reader;
    public static Runnable action;
    public static Failure failure;
    public static void hit(int event){events=events*10+event;if(failAt==event)throw failure;}
}
