package fixture;
public class Trace {
    public static int calls,index,failAt;
    public static long events;
    public static Class<?> type;
    public static Object payload;
    public static Enum<?> result;
    public static Runnable action;
    public static RuntimeException failure;
    public static void hit(int event){events=events*10+event;if(failAt==event)throw failure;}
}
