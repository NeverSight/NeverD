package fixture;
public class Trace {
    public static int calls,actions,failAt;
    public static long events;
    public static Object[] input,data,result;
    public static String message;
    public static Object marker;
    public static Runnable action;
    public static RuntimeException failure;
    public static void hit(int event){events=events*10+event;if(event==failAt)throw failure;}
}
