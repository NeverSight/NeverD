package fixture;
public class Trace {
    public static int calls,failAt;
    public static long events;
    public static boolean result;
    public static Object[] values;
    public static Object payload;
    public static Runnable action;
    public static RuntimeException failure;
    public static void hit(int event){events=events*10+event;if(failAt==event)throw failure;}
}
