package fixture;
public class Trace {
    public static int calls,failAt,dispatch;
    public static long events,count;
    public static Object receiver,result,payload;
    public static Value values;
    public static boolean flag;
    public static Action action;
    public static Failure failure;
    public static void hit(int event) throws Failure {events=events*10+event;if(failAt==event)throw failure;}
}
