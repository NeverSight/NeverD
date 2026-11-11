package fixture;
public class Trace {
    public static int events,index;
    public static boolean apply;
    public static Object source,mapper,mapped,result;
    public static Class<?> type;
    public static Runnable action;
    public static void hit(int event) { events=events*10+event; }
}
