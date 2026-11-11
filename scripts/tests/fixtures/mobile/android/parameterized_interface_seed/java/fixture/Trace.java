package fixture;
public class Trace {
    public static int calls,failAt,dispatch;
    public static long events;
    public static Object receiver,result,payload;
    public static java.util.Collection<String> values;
    public static boolean flag;
    public static Runnable action;
    public static Failure failure;
    public static void hit(int event){events=events*10+event;if(failAt==event)throw failure;}
}
