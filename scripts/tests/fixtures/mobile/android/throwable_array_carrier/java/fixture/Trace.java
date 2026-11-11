package fixture;
public class Trace {
    public static int calls,actions,failAt;
    public static long events;
    public static Object[] values,result;
    public static Object provider,marker;
    public static boolean reject;
    public static Runnable action;
    public static RuntimeException failure;
    public static void hit(int event){events=events*10+event;if(event==failAt)throw failure;}
}
