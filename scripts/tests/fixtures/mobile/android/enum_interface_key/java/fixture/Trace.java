package fixture;
public class Trace {
    public static int calls,failAt;
    public static long events;
    public static Object receiver,key,payload;
    public static Class<?> type;
    public static Enum<?> fallback,result;
    public static Runnable action;
    public static RuntimeException failure;
    public static void hit(int event){events=events*10+event;if(failAt==event)throw failure;}
}
