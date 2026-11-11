package fixture;
import java.io.File;
import java.lang.Iterable;
public class Trace {
    public static int calls,failAt,dispatch;
    public static long events;
    public static Iterable<?> values;
    public static File file,result;
    public static Thread.State context;
    public static boolean flag;
    public static Object payload,receiver;
    public static Runnable action;
    public static Failure failure;
    public static void hit(int event){events=events*10+event;if(failAt==event)throw failure;}
}
