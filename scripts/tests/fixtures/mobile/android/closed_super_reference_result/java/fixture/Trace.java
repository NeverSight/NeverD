package fixture;
import java.io.IOException;
public class Trace {
    public static long events;
    public static int failAt;
    public static Object value,items,result;
    public static IOException error;
    public static Runnable action;
    public static RuntimeException failure;
    public static void hit(int event){events=events*10+event;if(event==failAt)throw failure;}
}
