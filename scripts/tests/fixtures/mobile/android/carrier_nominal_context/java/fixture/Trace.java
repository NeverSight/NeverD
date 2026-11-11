package fixture;
import java.io.File;
import java.util.Collection;
public class Trace {
    public static int calls,failAt;
    public static long events;
    public static Collection<?> values;
    public static File file,result;
    public static String text;
    public static boolean flag;
    public static Object payload;
    public static Runnable action;
    public static Failure failure;
    public static void hit(int event){events=events*10+event;if(failAt==event)throw failure;}
}
