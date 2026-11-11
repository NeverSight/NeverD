package fixture;
import java.io.File;
import java.util.Collection;
public class Trace {
    public static int calls,siblings,failAt;
    public static long events;
    public static String[] data;
    public static Collection<File> values;
    public static Object payload,result;
    public static Runnable action,childAction;
    public static RuntimeException failure;
    public static void hit(int event){events=events*10+event;if(failAt==event)throw failure;}
}
