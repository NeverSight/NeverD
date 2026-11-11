package fixture;
import java.util.Map;
public class Trace {
    public static int calls,failAt;
    public static long events;
    public static Class<?> kind,keys,values;
    public static Object value,payload,read;
    public static Map<?,?> result;
    public static Runnable action,getAction;
    public static RuntimeException failure;
    public static void hit(int event) {events=events*10+event;if(failAt==event)throw failure;}
}
