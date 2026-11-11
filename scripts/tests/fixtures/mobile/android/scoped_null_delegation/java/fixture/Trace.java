package fixture;
public class Trace {
    public static long events;
    public static int actions,failAt;
    public static Link<?> link;
    public static Object payload;
    public static RuntimeException failure;
    public static Runnable action;
    public static void hit(int event){events=events*10+event;if(event==failAt)throw failure;}
}
