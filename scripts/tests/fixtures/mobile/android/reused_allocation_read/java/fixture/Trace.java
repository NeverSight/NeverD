package fixture;
public class Trace {
    public static int calls,failAt,targetInitializations,changerInitializations;
    public static long events;
    public static Object receiver,payload;
    public static String replacement,middle;
    public static Runnable action;
    public static RuntimeException failure;
    public static void hit(int event){events=events*10+event;if(failAt==event)throw failure;}
}
