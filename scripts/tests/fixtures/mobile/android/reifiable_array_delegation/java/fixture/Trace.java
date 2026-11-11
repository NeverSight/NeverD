package fixture;
import java.io.File;
import java.nio.file.Path;
public class Trace {
    public static int calls,conversions,failAt;
    public static long events;
    public static File receiver;
    public static Path path,converted;
    public static Class<?>[] data;
    public static Object payload,result;
    public static Runnable action,targetAction;
    public static RuntimeException failure;
    public static void hit(int event){events=events*10+event;if(event==failAt)throw failure;}
}
