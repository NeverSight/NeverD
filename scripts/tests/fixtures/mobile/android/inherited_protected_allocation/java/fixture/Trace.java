package fixture;
public class Trace {
    public static int calls,overrides,failAt;
    public static long events;
    public static Object value,payload;
    public static String result;
    public static Runnable action;
    public static RuntimeException failure;
    public static void hit(int event){events=events*10+event;if(failAt==event)throw failure;}
}
