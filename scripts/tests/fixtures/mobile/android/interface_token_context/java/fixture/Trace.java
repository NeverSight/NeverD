package fixture;
import java.io.File;
import java.io.IOException;
public class Trace {
    public static int calls,failAt;
    public static long events;
    public static CharSequence[] data;
    public static Class<?> type;
    public static File context;
    public static Object payload,result;
    public static Runnable action;
    public static RuntimeException failure;
    public static IOException checked;
    public static void hit(int event){events=events*10+event;if(failAt==event)throw failure;}
}
