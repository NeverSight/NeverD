package fixture;
public class Trace {
    public static int calls,actions,iterators,failAt;
    public static long events;
    public static Iterable<?> value,result;
    public static Object provider,marker;
    public static String message;
    public static Object[] data;
    public static boolean reject;
    public static Runnable action;
    public static RuntimeException failure;
    public static void hit(int event){events=events*10+event;if(event==failAt)throw failure;}
}
