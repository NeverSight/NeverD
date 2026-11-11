package fixture;
public class Trace {
    public static int calls,stage,observed,failAt;
    public static long events;
    public static Object[] results;
    public static Runnable action;
    public static NoSuchMethodException checked;
    public static void hit(int event){events=events*10+event;}
}
