package fixture;
public class Trace {
    public static int events;
    public static Object input,key,fallback,result,observed;
    public static Runnable action;
    public static void hit(int event) { events=events*10+event; }
}
