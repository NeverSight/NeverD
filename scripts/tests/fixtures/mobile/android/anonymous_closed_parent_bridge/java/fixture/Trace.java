package fixture;
public class Trace {
    public static int parents,children,futures,actions,observedLeft,observedRight;
    public static long events;
    public static float fraction;
    public static Parent actualReceiver;
    public static Object host,left,right,payload;
    public static Value result;
    public static Runnable action;
    public static void hit(int event){events=events*10+event;}
}
