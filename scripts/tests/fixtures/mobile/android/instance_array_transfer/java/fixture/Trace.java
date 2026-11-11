package fixture;
public class Trace {
    public static int calls,actions,dispatches,failAt;
    public static long events;
    public static Object[] input,result;
    public static Object receiver,marker;
    public static Runnable action;
    public static RuntimeException failure;
    public static void hit(int event){events=events*10+event;if(event==failAt)throw failure;}
}
